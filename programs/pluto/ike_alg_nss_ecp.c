/*
 * Copyright (C) 2017 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stddef.h>
#include <stdint.h>

#include "nspr.h"
#include "nss.h"
#include "pk11pub.h"
#include "keyhi.h"
/*
 * In addition to EC_POINT_FORM_UNCOMPRESSED, "blapit.h" things like
 * AES_BLOCK_SIZE which conflicts with "ietf_constants.h".
 */
#if 0
#include "blapit.h"
#else
#define EC_POINT_FORM_UNCOMPRESSED 0x04
#endif

#include "constants.h"
#include "lswalloc.h"
#include "test_buffer.h"
#include "lswnss.h"
#include "lswlog.h"

#include "ike_alg.h"
#include "ike_alg_nss_ecp.h"
#include "crypt_symkey.h"

static void nss_ecp_calc_ke(const struct oakley_group_desc *group,
			    SECKEYPrivateKey **privk,
			    SECKEYPublicKey **pubk,
			    uint8_t *ke, size_t sizeof_ke)
{
	passert(sizeof_ke == group->bytes);
	/*
	 * Get the PK11 formatted EC parameters (stored in static
	 * data) from NSS.
	 */
	DBG(DBG_CRYPT, DBG_log("oid %d %x", group->nss_oid, group->nss_oid));
	SECOidData *pk11_data = SECOID_FindOIDByTag(group->nss_oid);
	if (pk11_data == NULL) {
		PASSERT_FAIL("Lookup of OID %d for EC group %s parameters failed",
			     group->nss_oid, group->common.name);
	}
	DBG(DBG_CRYPT,
	    DBG_dump("pk11_data", pk11_data->oid.data, pk11_data->oid.len));

	/*
	 * Need to prepend the param with its size; for moment assume
	 * the returned value is small.  If it ever gets too big will
	 * need to re-encode the length some how.
	 */
	passert(pk11_data->oid.len < 256);
	SECKEYECParams *pk11_param = SECITEM_AllocItem(NULL, NULL, (2 + pk11_data->oid.len));
	pk11_param->type = siBuffer,
	pk11_param->data[0] = SEC_ASN1_OBJECT_ID;
	pk11_param->data[1] = pk11_data->oid.len;
	memcpy(pk11_param->data + 2, pk11_data->oid.data, pk11_data->oid.len);
	DBG(DBG_CRYPT,
	    DBG_dump("pk11_param", pk11_param->data, pk11_param->len));

	*privk = SECKEY_CreateECPrivateKey(pk11_param, pubk,
					   lsw_return_nss_password_file_info());

	SECITEM_FreeItem(pk11_param, PR_TRUE);

	if (*pubk == NULL || *privk == NULL) {
		PASSERT_FAIL("NSS: DH ECP private key creation failed (err %d)",
			     PR_GetError());
	}

	DBG(DBG_CRYPT,
	    DBG_log("public keyType %d size %d publicValue %p %d",
		    (*pubk)->keyType,
		    (*pubk)->u.ec.size,
		    (*pubk)->u.ec.publicValue.data,
		    (*pubk)->u.ec.publicValue.len);
	    DBG_dump("public key", (*pubk)->u.ec.publicValue.data,
		     (*pubk)->u.ec.publicValue.len));

	passert((*pubk)->u.ec.publicValue.data[0] == EC_POINT_FORM_UNCOMPRESSED);
	passert((*pubk)->u.ec.publicValue.len == group->bytes + 1);
	memcpy(ke, (*pubk)->u.ec.publicValue.data + 1, group->bytes);
}

static PK11SymKey *nss_ecp_calc_g_ir(const struct oakley_group_desc *group UNUSED,
				     SECKEYPrivateKey *local_privk,
				     const SECKEYPublicKey *local_pubk UNUSED,
				     uint8_t *remote_ke, size_t sizeof_remote_ke)
{
	passert(sizeof_remote_ke == group->bytes);
	passert(sizeof_remote_ke == local_pubk->u.ec.publicValue.len - 1);
	SECKEYPublicKey remote_pubk = {
		.keyType = ecKey,
		.u.ec = {
			.DEREncodedParams = local_pubk->u.ec.DEREncodedParams,
#if 0
			/*
			 * NSS, at one point, added the field
			 * .encoding and then removed it.  Building
			 * against one version and executing against
			 * the next will be 'bad'.
			 */
			.encoding = local_pubk->u.ec.encoding,
#endif
		},
	};

	SECITEM_AllocItem(NULL, &remote_pubk.u.ec.publicValue,
			  sizeof_remote_ke + 1),
	remote_pubk.u.ec.publicValue.data[0] = EC_POINT_FORM_UNCOMPRESSED;
	memcpy(remote_pubk.u.ec.publicValue.data + 1, remote_ke, sizeof_remote_ke);

	/*
	 * XXX: The "result type" can be nearly everything.  Use
	 * CKM_ECDH1_DERIVE as a marker so it is easy to spot this key
	 * type.
	 *
	 * Like all calls in the NSS source code, leave KDF=CKD_NULL.
	 * The raw key is also what CAVP tests expect.
	 */
	PK11SymKey *temp = PK11_PubDeriveWithKDF(local_privk, &remote_pubk,
						 /* is sender */ PR_FALSE,
						 /* secrets */ NULL, NULL,
						 /* Operation */ CKM_ECDH1_DERIVE,
						 /* result type */ CKM_ECDH1_DERIVE,
						 /* operation */ CKA_DERIVE,
						 /* key size */ 0,
						 /* KDF */ CKD_NULL,
						 /* shared data */ NULL,
						 /* ctx */ lsw_return_nss_password_file_info());
	DBG(DBG_CRYPT, DBG_symkey(__func__, "new temp", temp));

	/*
	 * The key returned above doesn't play well with PK11_Derive()
	 * - "softokn" fails to extract its value when trying to
	 * CKM_CONCATENATE_BASE_AND_KEY - work around this by
	 * returning a copy of the key.
	 */
	PK11SymKey *g_ir = key_from_symkey_bytes(temp, 0, sizeof_symkey(temp));
	DBG(DBG_CRYPT,
	    DBG_log("NSS: extracted-key@%p from ECDH temp-key@%p (CKM_CONCATENATE_BASE_AND_KEY hack)",
		    g_ir, temp));

	release_symkey(__func__, "temp", &temp);
	SECITEM_FreeItem(&remote_pubk.u.ec.publicValue, PR_FALSE);

	return g_ir;
}

struct dhmke_ops ike_alg_nss_ecp_dhmke_ops = {
	.calc_ke = nss_ecp_calc_ke,
	.calc_g_ir = nss_ecp_calc_g_ir,
};

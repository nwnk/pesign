/* Copyright 2012 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <nspr4/prtypes.h>
#include <nspr4/prerror.h>
#include <nspr4/prprf.h>

#include <nss3/nss.h>
#include <nss3/base64.h>
#include <nss3/cert.h>
#include <nss3/cryptohi.h>
#include <nss3/keyhi.h>
#include <nss3/secder.h>
#include <nss3/secerr.h>
#include <nss3/secport.h>
#include <nss3/secpkcs7.h>
#include <nss3/secoidt.h>
#include <nss3/pk11pub.h>

#include "cms_common.h"
#include "util.h"

typedef struct {
	SECItem data;
	SECAlgorithmID keytype;
	SECItem sig;
} SignedCert;

static SEC_ASN1Template SignedCertTemplate[] = {
	{.kind = SEC_ASN1_SEQUENCE,
	 .offset = 0,
	 .sub = NULL,
	 .size = sizeof(SignedCert),
	},
	{.kind = SEC_ASN1_ANY,
	 .offset = offsetof(SignedCert, data),
	 .sub = &SEC_AnyTemplate,
	 .size = sizeof (SECItem),
	},
	{.kind = SEC_ASN1_INLINE,
	 .offset = offsetof(SignedCert, keytype),
	 .sub = &SECOID_AlgorithmIDTemplate,
	 .size = sizeof (SECAlgorithmID),
	},
	{.kind = SEC_ASN1_OCTET_STRING,
	 .offset = offsetof(SignedCert, sig),
	 .sub = NULL,
	 .size = sizeof (SECItem),
	},
	{ 0, }
};

static int
bundle_signature(cms_context *cms, SECItem *sigder, SECItem *data,
		SECOidTag oid, SECItem *signature)
{
	SignedCert cert = {
		.data = {.data = data->data,
			 .len = data->len,
			 .type = data->type
		},
		.sig = {.data = calloc(1, signature->len + 1),
			.len = signature->len + 1,
			.type = signature->type
		}
	};

	memcpy((void *)cert.sig.data + 1, signature->data, signature->len);

	int rc = generate_algorithm_id(cms, &cert.keytype, oid);
	if (rc < 0)
		return -1;

	void *ret;
	ret = SEC_ASN1EncodeItem(NULL, sigder, &cert, SignedCertTemplate);
	if (ret == NULL)
		errx(1, "could not encode certificate: %s",
			PORT_ErrorToString(PORT_GetError()));

	sigder->data[sigder->len - 261] = DER_BIT_STRING;

	return 0;
}

static int
add_subject_key_id(cms_context *cms, void *extHandle, SECKEYPublicKey *pubkey)
{
	SECItem *pubkey_der = PK11_DEREncodePublicKey(pubkey);
	if (!pubkey_der)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	SECItem *encoded = PK11_MakeIDFromPubKey(pubkey_der);
	if (!encoded)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	/* for some reason PK11_MakeIDFromPubKey() doesn't generate the final
	 * wrapper for this... */
	SECItem wrapped = { 0 };
	int rc = generate_octet_string(cms, &wrapped, encoded);
	if (rc < 0)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	SECStatus status;
	status = CERT_AddExtension(extHandle, SEC_OID_X509_SUBJECT_KEY_ID,
					&wrapped, PR_FALSE, PR_TRUE);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	return 0;
}

static int
add_auth_key_id(cms_context *cms, void *extHandle, SECKEYPublicKey *pubkey)
{
	SECItem *pubkey_der = PK11_DEREncodePublicKey(pubkey);
	if (!pubkey_der)
		cmserr(-1, cms, "could not encode CA Key ID extension");

	SECItem *encoded = PK11_MakeIDFromPubKey(pubkey_der);
	if (!encoded)
		cmserr(-1, cms, "could not encode CA Key ID extension");

	SECItem cspecific = { 0 };
	int rc = make_context_specific(cms, 0, &cspecific, encoded);
	if (rc < 0)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	/* for some reason PK11_MakeIDFromPubKey() doesn't generate the final
	 * wrapper for this... */
	SECItem wrapped = { 0 };
	rc = wrap_in_seq(cms, &wrapped, &cspecific, 1);
	if (rc < 0)
		cmsreterr(-1, cms, "could not encode subject key id extension");

	SECStatus status;
	status = CERT_AddExtension(extHandle, SEC_OID_X509_AUTH_KEY_ID,
					&wrapped, PR_FALSE, PR_TRUE);
	if (status != SECSuccess)
		cmserr(-1, cms, "could not encode CA Key ID extension");
	return 0;
}


static int
add_key_usage(cms_context *cms, void *extHandle)
{
	uint8_t value[4] = {0,0,0,0};
	SECItem bitStringValue;

#if 0
	value[3] = NS_CERT_TYPE_SSL_SERVER |
		   NS_CERT_TYPE_EMAIL_CA |
		   NS_CERT_TYPE_OBJECT_SIGNING_CA;

	while (!(value[3] & 0x8)) {
		value[3] <<= 1;
		value[2]++;
	}
#else
	value[3] = 0x86;
	value[2] = 0x01;
	value[1] = 0x02;
	value[0] = 0x03;
#endif

	bitStringValue.data = (unsigned char *)&value;
	bitStringValue.len = sizeof (value);

	SECStatus status;
	status = CERT_AddExtension(extHandle, SEC_OID_X509_KEY_USAGE,
					&bitStringValue, PR_TRUE, PR_TRUE);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode key usage extension");

	return 0;
}

static int
add_basic_constraints(cms_context *cms, void *extHandle)
{
	CERTBasicConstraints basicConstraint;
	basicConstraint.pathLenConstraint = CERT_UNLIMITED_PATH_CONSTRAINT;
	basicConstraint.isCA = PR_TRUE;

	SECStatus status;

	SECItem encoded;

	status = CERT_EncodeBasicConstraintValue(cms->arena, &basicConstraint,
					&encoded);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode basic constraints");

	status = CERT_AddExtension(extHandle, SEC_OID_X509_BASIC_CONSTRAINTS,
					&encoded, PR_TRUE, PR_TRUE);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode basic constraints");

	return 0;
}

static int
add_extended_key_usage(cms_context *cms, void *extHandle)
{
	SECItem value = {
		.data = (unsigned char *)"\x30\x0a\x06\x08\x2b\x06\x01"
					 "\x05\x05\x07\x03\x03",
		.len = 12,
		.type = siBuffer
	};


	SECStatus status;

	status = CERT_AddExtension(extHandle, SEC_OID_X509_EXT_KEY_USAGE,
					&value, PR_FALSE, PR_TRUE);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode extended key usage");

	return 0;
}

static int
add_auth_info(cms_context *cms, void *extHandle, char *url)
{
	SECItem value;
	int rc;

	rc = generate_auth_info(cms, &value, url);
	if (rc < 0)
		return rc;

	SECStatus status;

	status = CERT_AddExtension(extHandle, SEC_OID_X509_AUTH_INFO_ACCESS,
				&value, PR_FALSE, PR_TRUE);
	if (status != SECSuccess)
		cmsreterr(-1, cms, "could not encode key authority information "
				"access extension");

	return 0;
}

static int
add_extensions_to_crq(cms_context *cms, CERTCertificateRequest *crq,
			int is_ca, int is_self_signed, SECKEYPublicKey *pubkey,
			SECKEYPublicKey *spubkey,
			char *url)
{
	void *mark = PORT_ArenaMark(cms->arena);

	void *extHandle;
	int rc;
	extHandle = CERT_StartCertificateRequestAttributes(crq);
	if (!extHandle)
		cmsreterr(-1, cms, "could not generate certificate extensions");

	rc = add_subject_key_id(cms, extHandle, pubkey);
	if (rc < 0)
		cmsreterr(-1, cms, "could not generate certificate extensions");

	if (is_ca) {
		rc = add_basic_constraints(cms, extHandle);
		if (rc < 0)
			cmsreterr(-1, cms, "could not generate certificate "
					"extensions");

		rc = add_key_usage(cms, extHandle);
		if (rc < 0)
			cmsreterr(-1, cms, "could not generate certificate extensions");
	}

	rc = add_extended_key_usage(cms, extHandle);
	if (rc < 0)
		cmsreterr(-1, cms, "could not generate certificate extensions");

	if (is_self_signed)
		rc = add_auth_key_id(cms, extHandle, pubkey);
	else
		rc = add_auth_key_id(cms, extHandle, spubkey);
	if (rc < 0)
		cmsreterr(-1, cms, "could not generate certificate extensions");

	rc = add_auth_info(cms, extHandle, url);
	if (rc < 0)
		cmsreterr(-1, cms, "could not generate certificate extensions");

	CERT_FinishExtensions(extHandle);
	CERT_FinishCertificateRequestAttributes(crq);
	PORT_ArenaUnmark(cms->arena, mark);
	return 0;
}

static int
populate_extensions(cms_context *cms, CERTCertificate *cert,
			CERTCertificateRequest *crq)
{
	CERTAttribute *attr = NULL;
	SECOidData *oid;

	oid = SECOID_FindOIDByTag(SEC_OID_PKCS9_EXTENSION_REQUEST);

	for (int i; crq->attributes[i]; i++) {
		attr = crq->attributes[i];
		if (attr->attrType.len != oid->oid.len)
			continue;
		if (!memcmp(attr->attrType.data, oid->oid.data, oid->oid.len))
			break;
		attr = NULL;
	}

	if (!attr)
		cmsreterr(-1, cms, "could not find extension request");

	SECStatus rv;
	rv = SEC_QuickDERDecodeItem(cms->arena, &cert->extensions,
				CERT_SequenceOfCertExtensionTemplate,
				*attr->attrValue);
	if (rv != SECSuccess)
		cmsreterr(-1, cms, "could not decode certificate extensions");
	return 0;
}

int main(int argc, char *argv[])
{
	int is_ca = 0;
	int is_self_signed = -1;
	char *tokenname = "NSS Certificate DB";
	char *signer = NULL;
	char *outfile = "signed.cer";
	char *poutfile = NULL;
	char *pubfile = NULL;
	char *cn = NULL;
	char *url = NULL;
	char *serial_str = NULL;
	char *issuer;
	unsigned long long serial = ULONG_MAX;

	cms_context *cms = NULL;

	poptContext optCon;
	struct poptOption options[] = {
		{NULL, '\0', POPT_ARG_INTL_DOMAIN, "pesign" },
		{"ca", 'C', POPT_ARG_VAL|POPT_ARGFLAG_DOC_HIDDEN, &is_ca, 1,
			"Generate a CA certificate", NULL },
		{"self-sign", 'S', POPT_ARG_VAL|POPT_ARGFLAG_DOC_HIDDEN,
			&is_self_signed, 1,
			"Generate a self-signed certificate", NULL },
		{"signer", 'c', POPT_ARG_STRING, &signer, 0,
			"Nickname for signing certificate", "<signer>" },
		{"token", 't', POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT,
			&tokenname, 0, "NSS token holding signing key",
			"<token>" },
		{"pubkey", 'p', POPT_ARG_STRING, &pubfile, 0,
			"Use public key from file", "<pubkey>" },
		{"output", 'o', POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT,
			&outfile, 0, "Certificate output file name",
			"<outfile>" },
		{"privkey", 'P', POPT_ARG_STRING,
			&poutfile, 0, "Private key output file name",
			"<privkey>" },
		{"common-name", 'n', POPT_ARG_STRING, &cn, 0,
			"Common Name for generated certificate", "<cn>" },
		{"url", 'u', POPT_ARG_STRING, &url, 0,
			"Issuer URL", "<url>" },
		{"serial", 's', POPT_ARG_STRING, &serial_str, 0,
			"Serial number", "<serial>" },
		{"issuer", 'i', POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
			&issuer, 0, "Issuer", "<issuer>" },
		POPT_AUTOALIAS
		POPT_AUTOHELP
		POPT_TABLEEND
	};

	optCon = poptGetContext("pesign", argc, (const char **)argv, options,0);

	int rc = poptReadDefaultConfig(optCon, 0);
	if (rc < 0)
		errx(1, "efikeygen: poptReadDefaultConfig failed: %s",
			poptStrerror(rc));

	while ((rc = poptGetNextOpt(optCon)) > 0)
		;

	if (rc < -1)
		errx(1, "efikeygen: invalid argument: %s: %s",
			poptBadOption(optCon, 0), poptStrerror(rc));

	if (poptPeekArg(optCon))
		errx(1, "efikeygen: invalid Argument: \"%s\"",
			poptPeekArg(optCon));

	poptFreeContext(optCon);

	if (is_self_signed == -1)
		is_self_signed = is_ca && !signer ? 1 : 0;

	if (is_self_signed && signer)
		errx(1, "efikeygen: --self-sign and --signer cannot be "
			"used at the same time.");

	if (!cn)
		errx(1, "efikeygen: --common-name must be specified");

	if (!is_self_signed && !signer)
		errx(1, "efikeygen: signing certificate is required");

	int outfd = open(outfile, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
	if (outfd < 0)
		err(1, "efikeygen: could not open \"%s\":", outfile);

	int p12fd = open(poutfile, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
	if (outfd < 0)
		err(1, "efikeygen: could not open \"%s\":", poutfile);

	SECItem pubkey = {
		.type = siBuffer,
		.data = NULL,
		.len = -1
	};

	if (pubfile) {
		int pubfd = open(pubfile, O_RDONLY);

		char *data = NULL;
		size_t *len = (size_t *)&pubkey.len;

		rc = read_file(pubfd, &data, len);
		if (rc < 0)
			err(1, "efikeygen: %s:%s:%d: could not read public "
				"key", __FILE__, __func__, __LINE__);
		close(pubfd);
		pubkey.data = (unsigned char *)data;
	}

	rc = cms_context_alloc(&cms);
	if (rc < 0)
		err(1, "efikeygen: %s:%d: could not allocate cms context:",
			__func__, __LINE__);

	if (tokenname) {
		cms->tokenname = strdup(tokenname);
		if (!cms->tokenname)
			err(1, "efikeygen: %s:%d could not allocate cms "
				"context:", __func__, __LINE__);
	}
	if (signer) {
		cms->certname = strdup(signer);
		if (!cms->certname)
			err(1, "efikeygen: %s:%d could not allocate cms "
				"context:", __func__, __LINE__);
	}

	SECStatus status = NSS_InitReadWrite("/etc/pki/pesign");
	if (status != SECSuccess)
		errx(1, "efikeygen: could not initialize NSS: %s",
			PORT_ErrorToString(PORT_GetError()));
	atexit((void (*)(void))NSS_Shutdown);

	if (!is_self_signed) {
		rc = find_certificate(cms);
		if (rc < 0)
			errx(1, "efikeygen: could not find signing "
				"certificate \"%s:%s\"", cms->tokenname,
				cms->certname);
	}

	errno = 0;
	serial = strtoull(serial_str, NULL, 0);
	if (errno == ERANGE && serial == ULLONG_MAX)
		err(1, "efikeygen: invalid serial number");

	SECItem certder;
	rc = generate_signing_certificate(cms, &certder, cn, is_ca,
				is_self_signed, url, serial,
				&pubkey);
	if (rc < 0)
		errx(1, "efikeygen: could not generate certificate");

	SECOidData *oid;
	oid = SECOID_FindOIDByTag(SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION);
	if (!oid)
		errx(1, "efikeygen: could not find OID for SHA256+RSA: %s",
			PORT_ErrorToString(PORT_GetError()));

	secuPWData pwdata_val = { 0, 0 };
	void *pwdata = &pwdata_val;
	SECKEYPrivateKey *privkey = PK11_FindKeyByAnyCert(cms->cert, pwdata);
	if (!privkey)
		errx(1, "efikeygen: could not find private key: %s",
			PORT_ErrorToString(PORT_GetError()));

	SECItem signature;
	status = SEC_SignData(&signature, certder.data, certder.len,
				privkey, oid->offset);

	SECItem sigder = { 0, };
	bundle_signature(cms, &sigder, &certder,
				SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION,
				&signature);

	rc = write(outfd, sigder.data, sigder.len);
	if (rc < 0) {
		save_errno(unlink(outfile));
		err(1, "efikeygen: could not write to %s", outfile);
	}
	close(outfd);

	close(p12fd);

	NSS_Shutdown();
	return 0;
}
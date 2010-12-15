/*
 * Copyright (C) 2000 Andreas Hess, Patric Lichtsteiner, Roger Wegmann
 * Copyright (C) 2001 Marco Bertossa, Andreas Schleiss
 * Copyright (C) 2002 Mario Strasser
 * Copyright (C) 2000-2006 Andreas Steffen
 * Copyright (C) 2006-2009 Martin Willi
 * Copyright (C) 2008 Tobias Brunner
 * Hochschule fuer Technik Rapperswil
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

#define _GNU_SOURCE

#include "x509_cert.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <library.h>
#include <debug.h>
#include <asn1/oid.h>
#include <asn1/asn1.h>
#include <asn1/asn1_parser.h>
#include <crypto/hashers/hasher.h>
#include <credentials/keys/private_key.h>
#include <utils/linked_list.h>
#include <utils/identification.h>
#include <selectors/traffic_selector.h>

/**
 * Different kinds of generalNames
 */
typedef enum {
	GN_OTHER_NAME =		0,
	GN_RFC822_NAME =	1,
	GN_DNS_NAME =		2,
	GN_X400_ADDRESS =	3,
	GN_DIRECTORY_NAME =	4,
	GN_EDI_PARTY_NAME = 5,
	GN_URI =			6,
	GN_IP_ADDRESS =		7,
	GN_REGISTERED_ID =	8,
} generalNames_t;


typedef struct private_x509_cert_t private_x509_cert_t;

/**
 * Private data of a x509_cert_t object.
 */
struct private_x509_cert_t {
	/**
	 * Public interface for this certificate.
	 */
	x509_cert_t public;

	/**
	 * X.509 certificate encoding in ASN.1 DER format
	 */
	chunk_t encoding;

	/**
	 * SHA1 hash of the DER encoding of this X.509 certificate
	 */
	chunk_t encoding_hash;

	/**
	 * X.509 certificate body over which signature is computed
	 */
	chunk_t tbsCertificate;

	/**
	 * Version of the X.509 certificate
	 */
	u_int version;

	/**
	 * Serial number of the X.509 certificate
	 */
	chunk_t serialNumber;

	/**
	 * ID representing the certificate issuer
	 */
	identification_t *issuer;

	/**
	 * Start time of certificate validity
	 */
	time_t notBefore;

	/**
	 * End time of certificate validity
	 */
	time_t notAfter;

	/**
	 * ID representing the certificate subject
	 */
	identification_t *subject;

	/**
	 * List of subjectAltNames as identification_t
	 */
	linked_list_t *subjectAltNames;

	/**
	 * List of crlDistributionPoints as crl_uri_t
	 */
	linked_list_t *crl_uris;

	/**
	 * List of ocspAccessLocations as allocated char*
	 */
	linked_list_t *ocsp_uris;

	/**
	 * List of ipAddrBlocks as traffic_selector_t
	 */
	linked_list_t *ipAddrBlocks;

	/**
	 * List of permitted name constraints
	 */
	linked_list_t *permitted_names;

	/**
	 * List of exluced name constraints
	 */
	linked_list_t *excluded_names;

	/**
	 * List of certificatePolicies, as x509_cert_policy_t
	 */
	linked_list_t *cert_policies;

	/**
	 * List of policyMappings, as x509_policy_mapping_t
	 */
	linked_list_t *policy_mappings;

	/**
	 * certificate's embedded public key
	 */
	public_key_t *public_key;

	/**
	 * Subject Key Identifier
	 */
	chunk_t subjectKeyIdentifier;

	/**
	 * Authority Key Identifier
	 */
	chunk_t authKeyIdentifier;

	/**
	 * Authority Key Serial Number
	 */
	chunk_t authKeySerialNumber;

	/**
	 * Path Length Constraint
	 */
	int pathLenConstraint;

	/**
	 * x509 constraints and other flags
	 */
	x509_flag_t flags;

	/**
	 * Signature algorithm
	 */
	int algorithm;

	/**
	 * Signature
	 */
	chunk_t signature;

	/**
	 * Certificate parsed from blob/file?
	 */
	bool parsed;

	/**
	 * reference count
	 */
	refcount_t ref;
};

static const chunk_t ASN1_subjectAltName_oid = chunk_from_chars(
	0x06, 0x03, 0x55, 0x1D, 0x11
);

/**
 * CRL URIs with associated issuer
 */
typedef struct {
	identification_t *issuer;
	linked_list_t *uris;
} crl_uri_t;

/**
 * Create a new issuer entry
 */
static crl_uri_t *crl_uri_create(identification_t *issuer)
{
	crl_uri_t *this;

	INIT(this,
		.issuer = issuer ? issuer->clone(issuer) : NULL,
		.uris = linked_list_create(),
	);
	return this;
}

/**
 * Destroy a CRL URI struct
 */
static void crl_uri_destroy(crl_uri_t *this)
{
	this->uris->destroy_function(this->uris, free);
	DESTROY_IF(this->issuer);
	free(this);
}

/**
 * Destroy a CertificatePolicy
 */
static void cert_policy_destroy(x509_cert_policy_t *this)
{
	free(this->oid.ptr);
	free(this->cps_uri);
	free(this->unotice_text);
	free(this);
}

/**
 * Free policy mapping
 */
static void policy_mapping_destroy(x509_policy_mapping_t *mapping)
{
	free(mapping->issuer.ptr);
	free(mapping->subject.ptr);
	free(mapping);
}

/**
 * ASN.1 definition of a basicConstraints extension
 */
static const asn1Object_t basicConstraintsObjects[] = {
	{ 0, "basicConstraints",	ASN1_SEQUENCE,	ASN1_NONE			}, /*  0 */
	{ 1,   "CA",				ASN1_BOOLEAN,	ASN1_DEF|ASN1_BODY	}, /*  1 */
	{ 1,   "pathLenConstraint",	ASN1_INTEGER,	ASN1_OPT|ASN1_BODY	}, /*  2 */
	{ 1,   "end opt",			ASN1_EOC,		ASN1_END  			}, /*  3 */
	{ 0, "exit",				ASN1_EOC,		ASN1_EXIT  			}
};
#define BASIC_CONSTRAINTS_CA		1
#define BASIC_CONSTRAINTS_PATH_LEN	2

/**
 * Extracts the basicConstraints extension
 */
static void parse_basicConstraints(chunk_t blob, int level0,
								   private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	bool isCA = FALSE;

	parser = asn1_parser_create(basicConstraintsObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case BASIC_CONSTRAINTS_CA:
				isCA = object.len && *object.ptr;
				DBG2(DBG_LIB, "  %s", isCA ? "TRUE" : "FALSE");
				if (isCA)
				{
					this->flags |= X509_CA;
				}
				break;
			case BASIC_CONSTRAINTS_PATH_LEN:
				if (isCA)
				{
					if (object.len == 0)
					{
						this->pathLenConstraint = 0;
					}
					else if (object.len == 1)
					{
						this->pathLenConstraint = *object.ptr;
					}
					/* we ignore path length constraints > 127 */
				}
				break;
			default:
				break;
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of otherName
 */
static const asn1Object_t otherNameObjects[] = {
	{0, "type-id",	ASN1_OID,			ASN1_BODY	}, /* 0 */
	{0, "value",	ASN1_CONTEXT_C_0,	ASN1_BODY	}, /* 1 */
	{0, "exit",		ASN1_EOC,			ASN1_EXIT	}
};
#define ON_OBJ_ID_TYPE		0
#define ON_OBJ_VALUE		1

/**
 * Extracts an otherName
 */
static bool parse_otherName(chunk_t *blob, int level0, id_type_t *type)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	int oid = OID_UNKNOWN;
	bool success = FALSE;

	parser = asn1_parser_create(otherNameObjects, *blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case ON_OBJ_ID_TYPE:
				oid = asn1_known_oid(object);
				break;
			case ON_OBJ_VALUE:
				switch (oid)
				{
					case OID_XMPP_ADDR:
						if (!asn1_parse_simple_object(&object, ASN1_UTF8STRING,
									parser->get_level(parser)+1, "xmppAddr"))
						{
							goto end;
						}
						break;
					case OID_USER_PRINCIPAL_NAME:
						if (asn1_parse_simple_object(&object, ASN1_UTF8STRING,
									parser->get_level(parser)+1, "msUPN"))
						{	/* we handle UPNs as RFC822 addr */
							*blob = object;
							*type = ID_RFC822_ADDR;
						}
						else
						{
							goto end;
						}
						break;
				}
				break;
			default:
				break;
		}
	}
	success = parser->success(parser);

end:
	parser->destroy(parser);
	return success;
}

/**
 * ASN.1 definition of generalName
 */
static const asn1Object_t generalNameObjects[] = {
	{ 0, "otherName",		ASN1_CONTEXT_C_0,  ASN1_OPT|ASN1_BODY	}, /*  0 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /*  1 */
	{ 0, "rfc822Name",		ASN1_CONTEXT_S_1,  ASN1_OPT|ASN1_BODY	}, /*  2 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END 			}, /*  3 */
	{ 0, "dnsName",			ASN1_CONTEXT_S_2,  ASN1_OPT|ASN1_BODY	}, /*  4 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /*  5 */
	{ 0, "x400Address",		ASN1_CONTEXT_S_3,  ASN1_OPT|ASN1_BODY	}, /*  6 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /*  7 */
	{ 0, "directoryName",	ASN1_CONTEXT_C_4,  ASN1_OPT|ASN1_BODY	}, /*  8 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /*  9 */
	{ 0, "ediPartyName",	ASN1_CONTEXT_C_5,  ASN1_OPT|ASN1_BODY	}, /* 10 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /* 11 */
	{ 0, "URI",				ASN1_CONTEXT_S_6,  ASN1_OPT|ASN1_BODY	}, /* 12 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /* 13 */
	{ 0, "ipAddress",		ASN1_CONTEXT_S_7,  ASN1_OPT|ASN1_BODY	}, /* 14 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /* 15 */
	{ 0, "registeredID",	ASN1_CONTEXT_S_8,  ASN1_OPT|ASN1_BODY	}, /* 16 */
	{ 0, "end choice",		ASN1_EOC,          ASN1_END				}, /* 17 */
	{ 0, "exit",			ASN1_EOC,          ASN1_EXIT			}
};
#define GN_OBJ_OTHER_NAME		 0
#define GN_OBJ_RFC822_NAME		 2
#define GN_OBJ_DNS_NAME			 4
#define GN_OBJ_X400_ADDRESS		 6
#define GN_OBJ_DIRECTORY_NAME	 8
#define GN_OBJ_EDI_PARTY_NAME	10
#define GN_OBJ_URI				12
#define GN_OBJ_IP_ADDRESS		14
#define GN_OBJ_REGISTERED_ID	16

/**
 * Extracts a generalName
 */
static identification_t *parse_generalName(chunk_t blob, int level0)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID ;

	identification_t *gn = NULL;

	parser = asn1_parser_create(generalNameObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		id_type_t id_type = ID_ANY;

		switch (objectID)
		{
			case GN_OBJ_RFC822_NAME:
				id_type = ID_RFC822_ADDR;
				break;
			case GN_OBJ_DNS_NAME:
				id_type = ID_FQDN;
				break;
			case GN_OBJ_URI:
				id_type = ID_DER_ASN1_GN_URI;
				break;
			case GN_OBJ_DIRECTORY_NAME:
				id_type = ID_DER_ASN1_DN;
				break;
			case GN_OBJ_IP_ADDRESS:
				switch (object.len)
				{
					case 4:
						id_type = ID_IPV4_ADDR;
						break;
					case 16:
						id_type = ID_IPV6_ADDR;
						break;
					default:
						break;
				}
				break;
			case GN_OBJ_OTHER_NAME:
				if (!parse_otherName(&object, parser->get_level(parser)+1,
									 &id_type))
				{
					goto end;
				}
				break;
			case GN_OBJ_X400_ADDRESS:
			case GN_OBJ_EDI_PARTY_NAME:
			case GN_OBJ_REGISTERED_ID:
			default:
				break;
		}
		if (id_type != ID_ANY)
		{
			gn = identification_create_from_encoding(id_type, object);
			DBG2(DBG_LIB, "  '%Y'", gn);
			goto end;
		}
	}

end:
	parser->destroy(parser);
	return gn;
}

/**
 * ASN.1 definition of generalNames
 */
static const asn1Object_t generalNamesObjects[] = {
	{ 0, "generalNames",	ASN1_SEQUENCE,	ASN1_LOOP }, /* 0 */
	{ 1,   "generalName",	ASN1_EOC,		ASN1_RAW  }, /* 1 */
	{ 0, "end loop",		ASN1_EOC,		ASN1_END  }, /* 2 */
	{ 0, "exit",			ASN1_EOC,		ASN1_EXIT }
};
#define GENERAL_NAMES_GN	1

/**
 * Extracts one or several GNs and puts them into a chained list
 */
void x509_parse_generalNames(chunk_t blob, int level0, bool implicit, linked_list_t *list)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;

	parser = asn1_parser_create(generalNamesObjects, blob);
	parser->set_top_level(parser, level0);
	parser->set_flags(parser, implicit, FALSE);

	while (parser->iterate(parser, &objectID, &object))
	{
		if (objectID == GENERAL_NAMES_GN)
		{
			identification_t *gn = parse_generalName(object,
											parser->get_level(parser)+1);

			if (gn)
			{
				list->insert_last(list, (void *)gn);
			}
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of a authorityKeyIdentifier extension
 */
static const asn1Object_t authKeyIdentifierObjects[] = {
	{ 0, "authorityKeyIdentifier",		ASN1_SEQUENCE,		ASN1_NONE 			}, /* 0 */
	{ 1,   "keyIdentifier",				ASN1_CONTEXT_S_0,	ASN1_OPT|ASN1_BODY	}, /* 1 */
	{ 1,   "end opt",					ASN1_EOC,			ASN1_END  			}, /* 2 */
	{ 1,   "authorityCertIssuer",		ASN1_CONTEXT_C_1,	ASN1_OPT|ASN1_OBJ	}, /* 3 */
	{ 1,   "end opt",					ASN1_EOC,			ASN1_END  			}, /* 4 */
	{ 1,   "authorityCertSerialNumber",	ASN1_CONTEXT_S_2,	ASN1_OPT|ASN1_BODY	}, /* 5 */
	{ 1,   "end opt",					ASN1_EOC,			ASN1_END  			}, /* 6 */
	{ 0, "exit",						ASN1_EOC,			ASN1_EXIT  			}
};
#define AUTH_KEY_ID_KEY_ID			1
#define AUTH_KEY_ID_CERT_ISSUER		3
#define AUTH_KEY_ID_CERT_SERIAL		5

/**
 * Extracts an authoritykeyIdentifier
 */
chunk_t x509_parse_authorityKeyIdentifier(chunk_t blob, int level0,
												chunk_t *authKeySerialNumber)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	chunk_t authKeyIdentifier = chunk_empty;

	*authKeySerialNumber = chunk_empty;

	parser = asn1_parser_create(authKeyIdentifierObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case AUTH_KEY_ID_KEY_ID:
				authKeyIdentifier = chunk_clone(object);
				break;
			case AUTH_KEY_ID_CERT_ISSUER:
				/* TODO: x509_parse_generalNames(object, level+1, TRUE); */
				break;
			case AUTH_KEY_ID_CERT_SERIAL:
				*authKeySerialNumber = object;
				break;
			default:
				break;
		}
	}
	parser->destroy(parser);
	return authKeyIdentifier;
}

/**
 * ASN.1 definition of a authorityInfoAccess extension
 */
static const asn1Object_t authInfoAccessObjects[] = {
	{ 0, "authorityInfoAccess",	ASN1_SEQUENCE,	ASN1_LOOP }, /* 0 */
	{ 1,   "accessDescription",	ASN1_SEQUENCE,	ASN1_NONE }, /* 1 */
	{ 2,     "accessMethod",	ASN1_OID,		ASN1_BODY }, /* 2 */
	{ 2,     "accessLocation",	ASN1_EOC,		ASN1_RAW  }, /* 3 */
	{ 0, "end loop",			ASN1_EOC,		ASN1_END  }, /* 4 */
	{ 0, "exit",				ASN1_EOC,		ASN1_EXIT }
};
#define AUTH_INFO_ACCESS_METHOD		2
#define AUTH_INFO_ACCESS_LOCATION	3

/**
 * Extracts an authorityInfoAcess location
 */
static void parse_authorityInfoAccess(chunk_t blob, int level0,
									  private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	int accessMethod = OID_UNKNOWN;

	parser = asn1_parser_create(authInfoAccessObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case AUTH_INFO_ACCESS_METHOD:
				accessMethod = asn1_known_oid(object);
				break;
			case AUTH_INFO_ACCESS_LOCATION:
			{
				switch (accessMethod)
				{
					case OID_OCSP:
					case OID_CA_ISSUERS:
						{
							identification_t *id;
							char *uri;

							id = parse_generalName(object,
											parser->get_level(parser)+1);
							if (id == NULL)
							{
								/* parsing went wrong - abort */
								goto end;
							}
							DBG2(DBG_LIB, "  '%Y'", id);
							if (accessMethod == OID_OCSP &&
								asprintf(&uri, "%Y", id) > 0)
							{
								this->ocsp_uris->insert_last(this->ocsp_uris, uri);
							}
							id->destroy(id);
						}
						break;
					default:
						/* unkown accessMethod, ignoring */
						break;
				}
				break;
			}
			default:
				break;
		}
	}

end:
	parser->destroy(parser);
}

/**
 * Extract KeyUsage flags
 */
static void parse_keyUsage(chunk_t blob, private_x509_cert_t *this)
{
	enum {
		KU_DIGITAL_SIGNATURE =	0,
		KU_NON_REPUDIATION =	1,
		KU_KEY_ENCIPHERMENT =	2,
		KU_DATA_ENCIPHERMENT =	3,
		KU_KEY_AGREEMENT =		4,
		KU_KEY_CERT_SIGN =		5,
		KU_CRL_SIGN =			6,
		KU_ENCIPHER_ONLY =		7,
		KU_DECIPHER_ONLY =		8,
	};

	if (asn1_unwrap(&blob, &blob) == ASN1_BIT_STRING && blob.len)
	{
		int bit, byte, unused = blob.ptr[0];

		blob = chunk_skip(blob, 1);
		for (byte = 0; byte < blob.len; byte++)
		{
			for (bit = 0; bit < 8; bit++)
			{
				if (byte == blob.len - 1 && bit > (7 - unused))
				{
					break;
				}
				if (blob.ptr[byte] & 1 << (7 - bit))
				{
					switch (byte * 8 + bit)
					{
						case KU_CRL_SIGN:
							this->flags |= X509_CRL_SIGN;
							break;
						case KU_KEY_CERT_SIGN:
							/* we use the caBasicConstraint, MUST be set */
						case KU_DIGITAL_SIGNATURE:
						case KU_NON_REPUDIATION:
						case KU_KEY_ENCIPHERMENT:
						case KU_DATA_ENCIPHERMENT:
						case KU_KEY_AGREEMENT:
						case KU_ENCIPHER_ONLY:
						case KU_DECIPHER_ONLY:
							break;
					}
				}
			}
		}
	}
}

/**
 * ASN.1 definition of a extendedKeyUsage extension
 */
static const asn1Object_t extendedKeyUsageObjects[] = {
	{ 0, "extendedKeyUsage",	ASN1_SEQUENCE,	ASN1_LOOP }, /* 0 */
	{ 1,   "keyPurposeID",		ASN1_OID,		ASN1_BODY }, /* 1 */
	{ 0, "end loop",			ASN1_EOC,		ASN1_END  }, /* 2 */
	{ 0, "exit",				ASN1_EOC,		ASN1_EXIT }
};
#define EXT_KEY_USAGE_PURPOSE_ID	1

/**
 * Extracts extendedKeyUsage OIDs
 */
static void parse_extendedKeyUsage(chunk_t blob, int level0,
								   private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;

	parser = asn1_parser_create(extendedKeyUsageObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		if (objectID == EXT_KEY_USAGE_PURPOSE_ID)
		{
			switch (asn1_known_oid(object))
			{
				case OID_SERVER_AUTH:
					this->flags |= X509_SERVER_AUTH;
					break;
				case OID_CLIENT_AUTH:
					this->flags |= X509_CLIENT_AUTH;
					break;
				case OID_OCSP_SIGNING:
					this->flags |= X509_OCSP_SIGNER;
					break;
				default:
					break;
			}
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of crlDistributionPoints
 */
static const asn1Object_t crlDistributionPointsObjects[] = {
	{ 0, "crlDistributionPoints",	ASN1_SEQUENCE,		ASN1_LOOP			}, /*  0 */
	{ 1,   "DistributionPoint",		ASN1_SEQUENCE,		ASN1_NONE			}, /*  1 */
	{ 2,     "distributionPoint",	ASN1_CONTEXT_C_0,	ASN1_OPT|ASN1_LOOP	}, /*  2 */
	{ 3,       "fullName",			ASN1_CONTEXT_C_0,	ASN1_OPT|ASN1_OBJ	}, /*  3 */
	{ 3,       "end choice",		ASN1_EOC,			ASN1_END			}, /*  4 */
	{ 3,       "nameRelToCRLIssuer",ASN1_CONTEXT_C_1,	ASN1_OPT|ASN1_BODY	}, /*  5 */
	{ 3,       "end choice",		ASN1_EOC,			ASN1_END			}, /*  6 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /*  7 */
	{ 2,     "reasons",				ASN1_CONTEXT_C_1,	ASN1_OPT|ASN1_BODY	}, /*  8 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /*  9 */
	{ 2,     "crlIssuer",			ASN1_CONTEXT_C_2,	ASN1_OPT|ASN1_OBJ	}, /* 10 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /* 11 */
	{ 0, "end loop",				ASN1_EOC,			ASN1_END			}, /* 12 */
	{ 0, "exit",					ASN1_EOC,			ASN1_EXIT			}
};
#define CRL_DIST_POINTS				 1
#define CRL_DIST_POINTS_FULLNAME	 3
#define CRL_DIST_POINTS_ISSUER		10

/**
 * Extracts one or several crlDistributionPoints into a list
 */
static void parse_crlDistributionPoints(chunk_t blob, int level0,
										private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	crl_uri_t *entry = NULL;
	identification_t *id;
	char *uri;
	linked_list_t *list = linked_list_create();

	parser = asn1_parser_create(crlDistributionPointsObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case CRL_DIST_POINTS:
				entry = crl_uri_create(NULL);
				this->crl_uris->insert_last(this->crl_uris, entry);
				break;
			case CRL_DIST_POINTS_FULLNAME:
				if (entry)
				{
					x509_parse_generalNames(object, parser->get_level(parser)+1,
											TRUE, list);
					while (list->remove_last(list, (void**)&id) == SUCCESS)
					{
						if (asprintf(&uri, "%Y", id) > 0)
						{
							entry->uris->insert_last(entry->uris, uri);
						}
						id->destroy(id);
					}
				}
				break;
			case CRL_DIST_POINTS_ISSUER:
				if (entry)
				{
					x509_parse_generalNames(object, parser->get_level(parser)+1,
											TRUE, list);
					while (list->remove_last(list, (void**)&id) == SUCCESS)
					{
						if (!entry->issuer)
						{
							entry->issuer = id;
						}
						else
						{
							id->destroy(id);
						}
					}
				}
				break;
		}
	}
	parser->destroy(parser);
	list->destroy(list);
}

/**
 * ASN.1 definition of nameConstraints
 */
static const asn1Object_t nameConstraintsObjects[] = {
	{ 0, "nameConstraints",			ASN1_SEQUENCE,		ASN1_LOOP			}, /*  0 */
	{ 1,   "permittedSubtrees",		ASN1_CONTEXT_C_0,	ASN1_OPT|ASN1_LOOP	}, /*  1 */
	{ 2,     "generalSubtree",		ASN1_SEQUENCE,		ASN1_BODY			}, /*  2 */
	{ 1,   "end loop",				ASN1_EOC,			ASN1_END			}, /*  3 */
	{ 1,   "excludedSubtrees",		ASN1_CONTEXT_C_1,	ASN1_OPT|ASN1_LOOP	}, /*  4 */
	{ 2,     "generalSubtree",		ASN1_SEQUENCE,		ASN1_BODY			}, /*  5 */
	{ 1,   "end loop",				ASN1_EOC,			ASN1_END			}, /*  6 */
	{ 0, "end loop",				ASN1_EOC,			ASN1_END			}, /*  7 */
	{ 0, "exit",					ASN1_EOC,			ASN1_EXIT			}
};
#define NAME_CONSTRAINT_PERMITTED 2
#define NAME_CONSTRAINT_EXCLUDED  5

/**
 * Parse permitted/excluded nameConstraints
 */
static void parse_nameConstraints(chunk_t blob, int level0,
								  private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	identification_t *id;
	chunk_t object;
	int objectID;

	parser = asn1_parser_create(nameConstraintsObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case NAME_CONSTRAINT_PERMITTED:
				id = parse_generalName(object, parser->get_level(parser) + 1);
				if (id)
				{
					this->permitted_names->insert_last(this->permitted_names, id);
				}
				break;
			case NAME_CONSTRAINT_EXCLUDED:
				id = parse_generalName(object, parser->get_level(parser) + 1);
				if (id)
				{
					this->excluded_names->insert_last(this->excluded_names, id);
				}
				break;
			default:
				break;
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of a certificatePolicies extension
 */
static const asn1Object_t certificatePoliciesObject[] = {
	{ 0, "certificatePolicies",		ASN1_SEQUENCE,	ASN1_LOOP			}, /*  0 */
	{ 1,   "policyInformation",		ASN1_SEQUENCE,	ASN1_NONE			}, /*  1 */
	{ 2,     "policyId",			ASN1_OID,		ASN1_BODY			}, /*  2 */
	{ 2,     "qualifier",			ASN1_SEQUENCE,	ASN1_OPT|ASN1_BODY	}, /*  3 */
	{ 3,       "qualifierInfo",		ASN1_SEQUENCE,	ASN1_NONE			}, /*  4 */
	{ 4,         "qualifierId",		ASN1_OID,		ASN1_BODY			}, /*  5 */
	{ 4,         "cPSuri",			ASN1_IA5STRING,	ASN1_OPT|ASN1_BODY	}, /*  6 */
	{ 4,         "end choice",		ASN1_EOC,		ASN1_END			}, /*  7 */
	{ 4,         "userNotice",		ASN1_SEQUENCE,	ASN1_OPT|ASN1_NONE	}, /*  8 */
	{ 5,           "explicitText",	ASN1_EOC,		ASN1_RAW			}, /*  9 */
	{ 4,         "end choice",		ASN1_EOC,		ASN1_END			}, /* 10 */
	{ 2,      "end opt",			ASN1_EOC,		ASN1_END			}, /* 12 */
	{ 0, "end loop",				ASN1_EOC,		ASN1_END			}, /* 13 */
	{ 0, "exit",					ASN1_EOC,		ASN1_EXIT			}
};
#define CERT_POLICY_ID				2
#define CERT_POLICY_QUALIFIER_ID	5
#define CERT_POLICY_CPS_URI			6
#define CERT_POLICY_EXPLICIT_TEXT	9

/**
 * Parse certificatePolicies
 */
static void parse_certificatePolicies(chunk_t blob, int level0,
									  private_x509_cert_t *this)
{
	x509_cert_policy_t *policy = NULL;
	asn1_parser_t *parser;
	chunk_t object;
	int objectID, qualifier = OID_UNKNOWN;

	parser = asn1_parser_create(certificatePoliciesObject, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case CERT_POLICY_ID:
				INIT(policy,
					.oid = chunk_clone(object),
				);
				this->cert_policies->insert_last(this->cert_policies, policy);
				break;
			case CERT_POLICY_QUALIFIER_ID:
				qualifier = asn1_known_oid(object);
				break;
			case CERT_POLICY_CPS_URI:
				if (policy && !policy->cps_uri && object.len &&
					qualifier == OID_POLICY_QUALIFIER_CPS &&
					chunk_printable(object, NULL, 0))
				{
					policy->cps_uri = strndup(object.ptr, object.len);
				}
				break;
			case CERT_POLICY_EXPLICIT_TEXT:
				/* TODO */
				break;
			default:
				break;
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of a policyMappings extension
 */
static const asn1Object_t policyMappingsObjects[] = {
	{ 0, "policyMappings",			ASN1_SEQUENCE,	ASN1_LOOP			}, /*  0 */
	{ 1,   "policyMapping",			ASN1_SEQUENCE,	ASN1_NONE			}, /*  1 */
	{ 2,     "issuerPolicy",		ASN1_OID,		ASN1_BODY			}, /*  2 */
	{ 2,     "subjectPolicy",		ASN1_OID,		ASN1_BODY			}, /*  3 */
	{ 0, "end loop",				ASN1_EOC,		ASN1_END			}, /*  4 */
	{ 0, "exit",					ASN1_EOC,		ASN1_EXIT			}
};
#define POLICY_MAPPING			1
#define POLICY_MAPPING_ISSUER	2
#define POLICY_MAPPING_SUBJECT	3

/**
 * Parse policyMappings
 */
static void parse_policyMappings(chunk_t blob, int level0,
								 private_x509_cert_t *this)
{
	x509_policy_mapping_t *map = NULL;
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;

	parser = asn1_parser_create(policyMappingsObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case POLICY_MAPPING:
				INIT(map);
				this->policy_mappings->insert_last(this->policy_mappings, map);
				break;
			case POLICY_MAPPING_ISSUER:
				if (map && !map->issuer.len)
				{
					map->issuer = chunk_clone(object);
				}
				break;
			case POLICY_MAPPING_SUBJECT:
				if (map && !map->subject.len)
				{
					map->subject = chunk_clone(object);
				}
				break;
			default:
				break;
		}
	}
	parser->destroy(parser);
}

/**
 * ASN.1 definition of ipAddrBlocks according to RFC 3779
 */
static const asn1Object_t ipAddrBlocksObjects[] = {
	{ 0, "ipAddrBlocks",	        ASN1_SEQUENCE,		ASN1_LOOP			}, /*  0 */
	{ 1,   "ipAddressFamily",		ASN1_SEQUENCE,		ASN1_NONE			}, /*  1 */
	{ 2,     "addressFamily",	    ASN1_OCTET_STRING,	ASN1_BODY          	}, /*  2 */
	{ 2,     "inherit",             ASN1_NULL,          ASN1_OPT|ASN1_NONE  }, /*  3 */
	{ 2,     "end choice",          ASN1_EOC,           ASN1_END            }, /*  4 */
	{ 2,     "addressesOrRanges",	ASN1_SEQUENCE,	    ASN1_OPT|ASN1_LOOP 	}, /*  5 */
	{ 3,       "addressPrefix",	    ASN1_BIT_STRING,	ASN1_OPT|ASN1_BODY  }, /*  6 */
	{ 3,       "end choice",        ASN1_EOC,           ASN1_END            }, /*  7 */
	{ 3,       "addressRange",      ASN1_SEQUENCE,      ASN1_OPT|ASN1_NONE  }, /*  8 */
	{ 4,         "min",             ASN1_BIT_STRING,    ASN1_BODY           }, /*  9 */
	{ 4,         "max",             ASN1_BIT_STRING,    ASN1_BODY           }, /* 10 */
	{ 3,       "end choice",        ASN1_EOC,           ASN1_END            }, /* 11 */
	{ 2,     "end choice/loop",     ASN1_EOC,           ASN1_END            }, /* 12 */
	{ 0, "end loop",				ASN1_EOC,			ASN1_END			}, /* 13 */
	{ 0, "exit",					ASN1_EOC,			ASN1_EXIT			}
};
#define IP_ADDR_BLOCKS_FAMILY       2
#define IP_ADDR_BLOCKS_INHERIT      3
#define IP_ADDR_BLOCKS_PREFIX       6
#define IP_ADDR_BLOCKS_MIN          9
#define IP_ADDR_BLOCKS_MAX         10

static bool check_address_object(ts_type_t ts_type, chunk_t object)
{
	switch (ts_type)
	{
		case TS_IPV4_ADDR_RANGE:
			if (object.len > 5)
			{
				DBG1(DBG_LIB, "IPv4 address object is larger than 5 octets");
				return FALSE;
			}
			break;
		case TS_IPV6_ADDR_RANGE:
			if (object.len > 17)
			{
				DBG1(DBG_LIB, "IPv6 address object is larger than 17 octets");
				return FALSE;
			}
			break;
		default:
			DBG1(DBG_LIB, "unknown address family");
			return FALSE;
	}
	if (object.len == 0)
	{
		DBG1(DBG_LIB, "An ASN.1 bit string must contain at least the "
			 "initial octet");
		return FALSE;
	}
	if (object.len == 1 && object.ptr[0] != 0)
	{
		DBG1(DBG_LIB, "An empty ASN.1 bit string must contain a zero "
			 "initial octet");
		return FALSE;
	}
	if (object.ptr[0] > 7)
	{
		DBG1(DBG_LIB, "number of unused bits is too large");
		return FALSE;
	}
	return TRUE;
}

static void parse_ipAddrBlocks(chunk_t blob, int level0,
							   private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object, min_object;
	ts_type_t ts_type = 0;
	traffic_selector_t *ts;
	int objectID;

	parser = asn1_parser_create(ipAddrBlocksObjects, blob);
	parser->set_top_level(parser, level0);

	while (parser->iterate(parser, &objectID, &object))
	{
		switch (objectID)
		{
			case IP_ADDR_BLOCKS_FAMILY:
				ts_type = 0;
				if (object.len == 2 && object.ptr[0] == 0)
				{
					if (object.ptr[1] == 1)
					{
						ts_type = TS_IPV4_ADDR_RANGE;
					}
					else if (object.ptr[1] == 2)
					{
						ts_type = TS_IPV6_ADDR_RANGE;
					}
					else
					{
						break;
					}
					DBG2(DBG_LIB, "  %N", ts_type_name, ts_type);
				}
				break;
			case IP_ADDR_BLOCKS_INHERIT:
				DBG1(DBG_LIB, "inherit choice is not supported");
				break;
			case IP_ADDR_BLOCKS_PREFIX:
				if (!check_address_object(ts_type, object))
				{
					goto end;
				}
				ts = traffic_selector_create_from_rfc3779_format(ts_type,
													object, object);
				DBG2(DBG_LIB, "  %R", ts);
				this->ipAddrBlocks->insert_last(this->ipAddrBlocks, ts);
				break;
			case IP_ADDR_BLOCKS_MIN:
				if (!check_address_object(ts_type, object))
				{
					goto end;
				}
				min_object = object;
				break;
			case IP_ADDR_BLOCKS_MAX:
				if (!check_address_object(ts_type, object))
				{
					goto end;
				}
				ts = traffic_selector_create_from_rfc3779_format(ts_type,
													min_object, object);
				DBG2(DBG_LIB, "  %R", ts);
				this->ipAddrBlocks->insert_last(this->ipAddrBlocks, ts);
				break;
			default:
				break;
		}
	}
	this->flags |= X509_IP_ADDR_BLOCKS;

end:
	parser->destroy(parser);
}

/**
 * ASN.1 definition of an X.509v3 x509_cert
 */
static const asn1Object_t certObjects[] = {
	{ 0, "x509",					ASN1_SEQUENCE,		ASN1_OBJ			}, /*  0 */
	{ 1,   "tbsCertificate",		ASN1_SEQUENCE,		ASN1_OBJ			}, /*  1 */
	{ 2,     "DEFAULT v1",			ASN1_CONTEXT_C_0,	ASN1_DEF			}, /*  2 */
	{ 3,       "version",			ASN1_INTEGER,		ASN1_BODY			}, /*  3 */
	{ 2,     "serialNumber",		ASN1_INTEGER,		ASN1_BODY			}, /*  4 */
	{ 2,     "signature",			ASN1_EOC,			ASN1_RAW			}, /*  5 */
	{ 2,     "issuer",				ASN1_SEQUENCE,		ASN1_OBJ			}, /*  6 */
	{ 2,     "validity",			ASN1_SEQUENCE,		ASN1_NONE			}, /*  7 */
	{ 3,       "notBefore",			ASN1_EOC,			ASN1_RAW			}, /*  8 */
	{ 3,       "notAfter",			ASN1_EOC,			ASN1_RAW			}, /*  9 */
	{ 2,     "subject",				ASN1_SEQUENCE,		ASN1_OBJ			}, /* 10 */
	{ 2,     "subjectPublicKeyInfo",ASN1_SEQUENCE,		ASN1_RAW			}, /* 11 */
	{ 2,     "issuerUniqueID",		ASN1_CONTEXT_C_1,	ASN1_OPT			}, /* 12 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /* 13 */
	{ 2,     "subjectUniqueID",		ASN1_CONTEXT_C_2,	ASN1_OPT			}, /* 14 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /* 15 */
	{ 2,     "optional extensions",	ASN1_CONTEXT_C_3,	ASN1_OPT			}, /* 16 */
	{ 3,       "extensions",		ASN1_SEQUENCE,		ASN1_LOOP			}, /* 17 */
	{ 4,         "extension",		ASN1_SEQUENCE,		ASN1_NONE			}, /* 18 */
	{ 5,           "extnID",		ASN1_OID,			ASN1_BODY			}, /* 19 */
	{ 5,           "critical",		ASN1_BOOLEAN,		ASN1_DEF|ASN1_BODY	}, /* 20 */
	{ 5,           "extnValue",		ASN1_OCTET_STRING,	ASN1_BODY			}, /* 21 */
	{ 3,       "end loop",			ASN1_EOC,			ASN1_END			}, /* 22 */
	{ 2,     "end opt",				ASN1_EOC,			ASN1_END			}, /* 23 */
	{ 1,   "signatureAlgorithm",	ASN1_EOC,			ASN1_RAW			}, /* 24 */
	{ 1,   "signatureValue",		ASN1_BIT_STRING,	ASN1_BODY			}, /* 25 */
	{ 0, "exit",					ASN1_EOC,			ASN1_EXIT			}
};
#define X509_OBJ_TBS_CERTIFICATE				 1
#define X509_OBJ_VERSION						 3
#define X509_OBJ_SERIAL_NUMBER					 4
#define X509_OBJ_SIG_ALG						 5
#define X509_OBJ_ISSUER 						 6
#define X509_OBJ_NOT_BEFORE						 8
#define X509_OBJ_NOT_AFTER						 9
#define X509_OBJ_SUBJECT						10
#define X509_OBJ_SUBJECT_PUBLIC_KEY_INFO		11
#define X509_OBJ_OPTIONAL_EXTENSIONS			16
#define X509_OBJ_EXTN_ID						19
#define X509_OBJ_CRITICAL						20
#define X509_OBJ_EXTN_VALUE						21
#define X509_OBJ_ALGORITHM						24
#define X509_OBJ_SIGNATURE						25

/**
 * Parses an X.509v3 certificate
 */
static bool parse_certificate(private_x509_cert_t *this)
{
	asn1_parser_t *parser;
	chunk_t object;
	int objectID;
	int extn_oid = OID_UNKNOWN;
	int sig_alg  = OID_UNKNOWN;
	bool success = FALSE;
	bool critical = FALSE;

	parser = asn1_parser_create(certObjects, this->encoding);

	while (parser->iterate(parser, &objectID, &object))
	{
		u_int level = parser->get_level(parser)+1;

		switch (objectID)
		{
			case X509_OBJ_TBS_CERTIFICATE:
				this->tbsCertificate = object;
				break;
			case X509_OBJ_VERSION:
				this->version = (object.len) ? (1+(u_int)*object.ptr) : 1;
				if (this->version < 1 || this->version > 3)
				{
					DBG1(DBG_LIB, "X.509v%d not supported", this->version);
					goto end;
				}
				else
				{
					DBG2(DBG_LIB, "  X.509v%d", this->version);
				}
				break;
			case X509_OBJ_SERIAL_NUMBER:
				this->serialNumber = object;
				break;
			case X509_OBJ_SIG_ALG:
				sig_alg = asn1_parse_algorithmIdentifier(object, level, NULL);
				break;
			case X509_OBJ_ISSUER:
				this->issuer = identification_create_from_encoding(ID_DER_ASN1_DN, object);
				DBG2(DBG_LIB, "  '%Y'", this->issuer);
				break;
			case X509_OBJ_NOT_BEFORE:
				this->notBefore = asn1_parse_time(object, level);
				break;
			case X509_OBJ_NOT_AFTER:
				this->notAfter = asn1_parse_time(object, level);
				break;
			case X509_OBJ_SUBJECT:
				this->subject = identification_create_from_encoding(ID_DER_ASN1_DN, object);
				DBG2(DBG_LIB, "  '%Y'", this->subject);
				break;
			case X509_OBJ_SUBJECT_PUBLIC_KEY_INFO:
				DBG2(DBG_LIB, "-- > --");
				this->public_key = lib->creds->create(lib->creds, CRED_PUBLIC_KEY,
						KEY_ANY, BUILD_BLOB_ASN1_DER, object, BUILD_END);
				DBG2(DBG_LIB, "-- < --");
				if (this->public_key == NULL)
				{
					goto end;
				}
				break;
			case X509_OBJ_OPTIONAL_EXTENSIONS:
				if (this->version != 3)
				{
					DBG1(DBG_LIB, "Only X.509v3 certificates have extensions");
					goto end;
				}
				break;
			case X509_OBJ_EXTN_ID:
				extn_oid = asn1_known_oid(object);
				break;
			case X509_OBJ_CRITICAL:
				critical = object.len && *object.ptr;
				DBG2(DBG_LIB, "  %s", critical ? "TRUE" : "FALSE");
				break;
			case X509_OBJ_EXTN_VALUE:
			{
				switch (extn_oid)
				{
					case OID_SUBJECT_KEY_ID:
						if (!asn1_parse_simple_object(&object, ASN1_OCTET_STRING,
													  level, "keyIdentifier"))
						{
							goto end;
						}
						this->subjectKeyIdentifier = object;
						break;
					case OID_SUBJECT_ALT_NAME:
						x509_parse_generalNames(object, level, FALSE,
												this->subjectAltNames);
						break;
					case OID_BASIC_CONSTRAINTS:
						parse_basicConstraints(object, level, this);
						break;
					case OID_CRL_DISTRIBUTION_POINTS:
						parse_crlDistributionPoints(object, level, this);
						break;
					case OID_AUTHORITY_KEY_ID:
						this->authKeyIdentifier = x509_parse_authorityKeyIdentifier(object,
														level, &this->authKeySerialNumber);
						break;
					case OID_AUTHORITY_INFO_ACCESS:
						parse_authorityInfoAccess(object, level, this);
						break;
					case OID_KEY_USAGE:
						parse_keyUsage(object, this);
						break;
					case OID_EXTENDED_KEY_USAGE:
						parse_extendedKeyUsage(object, level, this);
						break;
					case OID_IP_ADDR_BLOCKS:
						parse_ipAddrBlocks(object, level, this);
						break;
					case OID_NAME_CONSTRAINTS:
						parse_nameConstraints(object, level, this);
						break;
					case OID_CERTIFICATE_POLICIES:
						parse_certificatePolicies(object, level, this);
						break;
					case OID_POLICY_MAPPINGS:
						parse_policyMappings(object, level, this);
						break;
					case OID_NS_REVOCATION_URL:
					case OID_NS_CA_REVOCATION_URL:
					case OID_NS_CA_POLICY_URL:
					case OID_NS_COMMENT:
						if (!asn1_parse_simple_object(&object, ASN1_IA5STRING,
											level, oid_names[extn_oid].name))
						{
							goto end;
						}
						break;
					default:
						if (critical && lib->settings->get_bool(lib->settings,
							"libstrongswan.plugins.x509.enforce_critical", FALSE))
						{
							DBG1(DBG_LIB, "critical %s extension not supported",
								 (extn_oid == OID_UNKNOWN) ? "unknown" :
								 (char*)oid_names[extn_oid].name);
							goto end;
						}
						break;
				}
				break;
			}
			case X509_OBJ_ALGORITHM:
				this->algorithm = asn1_parse_algorithmIdentifier(object, level, NULL);
				if (this->algorithm != sig_alg)
				{
					DBG1(DBG_LIB, "  signature algorithms do not agree");
					goto end;
				}
				break;
			case X509_OBJ_SIGNATURE:
				this->signature = object;
				break;
			default:
				break;
		}
	}
	success = parser->success(parser);

end:
	parser->destroy(parser);
	if (success)
	{
		hasher_t *hasher;

		/* check if the certificate is self-signed */
		if (this->public.interface.interface.issued_by(
											&this->public.interface.interface,
											&this->public.interface.interface))
		{
			this->flags |= X509_SELF_SIGNED;
		}
		/* create certificate hash */
		hasher = lib->crypto->create_hasher(lib->crypto, HASH_SHA1);
		if (hasher == NULL)
		{
			DBG1(DBG_LIB, "  unable to create hash of certificate, SHA1 not supported");
			return NULL;
		}
		hasher->allocate_hash(hasher, this->encoding, &this->encoding_hash);
		hasher->destroy(hasher);
	}
	return success;
}

METHOD(certificate_t, get_type, certificate_type_t,
	private_x509_cert_t *this)
{
	return CERT_X509;
}

METHOD(certificate_t, get_subject, identification_t*,
	private_x509_cert_t *this)
{
	return this->subject;
}

METHOD(certificate_t, get_issuer, identification_t*,
	private_x509_cert_t *this)
{
	return this->issuer;
}

METHOD(certificate_t, has_subject, id_match_t,
	private_x509_cert_t *this, identification_t *subject)
{
	identification_t *current;
	enumerator_t *enumerator;
	id_match_t match, best;
	chunk_t encoding;

	if (subject->get_type(subject) == ID_KEY_ID)
	{
		encoding = subject->get_encoding(subject);

		if (this->encoding_hash.len &&
			chunk_equals(this->encoding_hash, encoding))
		{
			return ID_MATCH_PERFECT;
		}
		if (this->subjectKeyIdentifier.len &&
			chunk_equals(this->subjectKeyIdentifier, encoding))
		{
			return ID_MATCH_PERFECT;
		}
		if (this->public_key &&
			this->public_key->has_fingerprint(this->public_key, encoding))
		{
			return ID_MATCH_PERFECT;
		}
	}
	best = this->subject->matches(this->subject, subject);
	enumerator = this->subjectAltNames->create_enumerator(this->subjectAltNames);
	while (enumerator->enumerate(enumerator, &current))
	{
		match = current->matches(current, subject);
		if (match > best)
		{
			best = match;
		}
	}
	enumerator->destroy(enumerator);
	return best;
}

METHOD(certificate_t, has_issuer, id_match_t,
	private_x509_cert_t *this, identification_t *issuer)
{
	/* issuerAltNames currently not supported */
	return this->issuer->matches(this->issuer, issuer);
}

METHOD(certificate_t, issued_by, bool,
	private_x509_cert_t *this, certificate_t *issuer)
{
	public_key_t *key;
	signature_scheme_t scheme;
	bool valid;
	x509_t *x509 = (x509_t*)issuer;

	if (&this->public.interface.interface == issuer)
	{
		if (this->flags & X509_SELF_SIGNED)
		{
			return TRUE;
		}
	}
	else
	{
		if (issuer->get_type(issuer) != CERT_X509)
		{
			return FALSE;
		}
		if (!(x509->get_flags(x509) & X509_CA))
		{
			return FALSE;
		}
	}
	if (!this->issuer->equals(this->issuer, issuer->get_subject(issuer)))
	{
		return FALSE;
	}

	/* determine signature scheme */
	scheme = signature_scheme_from_oid(this->algorithm);
	if (scheme == SIGN_UNKNOWN)
	{
		return FALSE;
	}
	/* get the public key of the issuer */
	key = issuer->get_public_key(issuer);
	if (!key)
	{
		return FALSE;
	}
	valid = key->verify(key, scheme, this->tbsCertificate, this->signature);
	key->destroy(key);
	return valid;
}

METHOD(certificate_t, get_public_key, public_key_t*,
	private_x509_cert_t *this)
{
	this->public_key->get_ref(this->public_key);
	return this->public_key;
}

METHOD(certificate_t, get_ref, certificate_t*,
	private_x509_cert_t *this)
{
	ref_get(&this->ref);
	return &this->public.interface.interface;
}

METHOD(certificate_t, get_validity, bool,
	private_x509_cert_t *this, time_t *when, time_t *not_before,
	time_t *not_after)
{
	time_t t = when ? *when : time(NULL);

	if (not_before)
	{
		*not_before = this->notBefore;
	}
	if (not_after)
	{
		*not_after = this->notAfter;
	}
	return (t >= this->notBefore && t <= this->notAfter);
}

METHOD(certificate_t, get_encoding, bool,
	private_x509_cert_t *this, cred_encoding_type_t type, chunk_t *encoding)
{
	if (type == CERT_ASN1_DER)
	{
		*encoding = chunk_clone(this->encoding);
		return TRUE;
	}
	return lib->encoding->encode(lib->encoding, type, NULL, encoding,
						CRED_PART_X509_ASN1_DER, this->encoding, CRED_PART_END);
}

METHOD(certificate_t, equals, bool,
	private_x509_cert_t *this, certificate_t *other)
{
	chunk_t encoding;
	bool equal;

	if (this == (private_x509_cert_t*)other)
	{
		return TRUE;
	}
	if (other->get_type(other) != CERT_X509)
	{
		return FALSE;
	}
	if (other->equals == (void*)equals)
	{	/* skip allocation if we have the same implementation */
		return chunk_equals(this->encoding, ((private_x509_cert_t*)other)->encoding);
	}
	if (!other->get_encoding(other, CERT_ASN1_DER, &encoding))
	{
		return FALSE;
	}
	equal = chunk_equals(this->encoding, encoding);
	free(encoding.ptr);
	return equal;
}

METHOD(x509_t, get_flags, x509_flag_t,
	private_x509_cert_t *this)
{
	return this->flags;
}

METHOD(x509_t, get_serial, chunk_t,
	private_x509_cert_t *this)
{
	return this->serialNumber;
}

METHOD(x509_t, get_subjectKeyIdentifier, chunk_t,
	private_x509_cert_t *this)
{
	if (this->subjectKeyIdentifier.ptr)
	{
		return this->subjectKeyIdentifier;
	}
	else
	{
		chunk_t fingerprint;

		if (this->public_key->get_fingerprint(this->public_key,
					 				KEYID_PUBKEY_SHA1, &fingerprint))
		{
			return fingerprint;
		}
		else
		{
			return chunk_empty;
		}
	}
}

METHOD(x509_t, get_authKeyIdentifier, chunk_t,
	private_x509_cert_t *this)
{
	return this->authKeyIdentifier;
}

METHOD(x509_t, get_pathLenConstraint, int,
	private_x509_cert_t *this)
{
	return this->pathLenConstraint;
}

METHOD(x509_t, create_subjectAltName_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return this->subjectAltNames->create_enumerator(this->subjectAltNames);
}

METHOD(x509_t, create_ocsp_uri_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return this->ocsp_uris->create_enumerator(this->ocsp_uris);
}

/**
 * Convert enumerator value from entry to (uri, issuer)
 */
static bool crl_enum_filter(identification_t *issuer_in,
							char **uri_in, char **uri_out,
							void *none_in, identification_t **issuer_out)
{
	*uri_out = *uri_in;
	if (issuer_out)
	{
		*issuer_out = issuer_in;
	}
	return TRUE;
}

/**
 * Create inner enumerator over URIs
 */
static enumerator_t *crl_enum_create(crl_uri_t *entry)
{
	return enumerator_create_filter(entry->uris->create_enumerator(entry->uris),
								(void*)crl_enum_filter, entry->issuer, NULL);
}

METHOD(x509_t, create_crl_uri_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return enumerator_create_nested(
							this->crl_uris->create_enumerator(this->crl_uris),
							(void*)crl_enum_create, NULL, NULL);
}

METHOD(x509_t, create_ipAddrBlock_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return this->ipAddrBlocks->create_enumerator(this->ipAddrBlocks);
}

METHOD(x509_t, create_name_constraint_enumerator, enumerator_t*,
	private_x509_cert_t *this, bool perm)
{
	if (perm)
	{
		return this->permitted_names->create_enumerator(this->permitted_names);
	}
	return this->excluded_names->create_enumerator(this->excluded_names);
}

METHOD(x509_t, create_cert_policy_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return this->cert_policies->create_enumerator(this->cert_policies);
}

METHOD(x509_t, create_policy_mapping_enumerator, enumerator_t*,
	private_x509_cert_t *this)
{
	return this->policy_mappings->create_enumerator(this->policy_mappings);
}

METHOD(certificate_t, destroy, void,
	private_x509_cert_t *this)
{
	if (ref_put(&this->ref))
	{
		this->subjectAltNames->destroy_offset(this->subjectAltNames,
									offsetof(identification_t, destroy));
		this->crl_uris->destroy_function(this->crl_uris, (void*)crl_uri_destroy);
		this->ocsp_uris->destroy_function(this->ocsp_uris, free);
		this->ipAddrBlocks->destroy_offset(this->ipAddrBlocks,
										offsetof(traffic_selector_t, destroy));
		this->permitted_names->destroy_offset(this->permitted_names,
										offsetof(identification_t, destroy));
		this->excluded_names->destroy_offset(this->excluded_names,
										offsetof(identification_t, destroy));
		this->cert_policies->destroy_function(this->cert_policies,
											  (void*)cert_policy_destroy);
		this->policy_mappings->destroy_function(this->policy_mappings,
											  (void*)policy_mapping_destroy);
		DESTROY_IF(this->issuer);
		DESTROY_IF(this->subject);
		DESTROY_IF(this->public_key);
		chunk_free(&this->authKeyIdentifier);
		chunk_free(&this->encoding);
		chunk_free(&this->encoding_hash);
		if (!this->parsed)
		{	/* only parsed certificates point these fields to "encoded" */
			chunk_free(&this->signature);
			chunk_free(&this->serialNumber);
			chunk_free(&this->tbsCertificate);
		}
		free(this);
	}
}

/**
 * create an empty but initialized X.509 certificate
 */
static private_x509_cert_t* create_empty(void)
{
	private_x509_cert_t *this;

	INIT(this,
		.public = {
			.interface = {
				.interface = {
					.get_type = _get_type,
					.get_subject = _get_subject,
					.get_issuer = _get_issuer,
					.has_subject = _has_subject,
					.has_issuer = _has_issuer,
					.issued_by = _issued_by,
					.get_public_key = _get_public_key,
					.get_validity = _get_validity,
					.get_encoding = _get_encoding,
					.equals = _equals,
					.get_ref = _get_ref,
					.destroy = _destroy,
				},
				.get_flags = _get_flags,
				.get_serial = _get_serial,
				.get_subjectKeyIdentifier = _get_subjectKeyIdentifier,
				.get_authKeyIdentifier = _get_authKeyIdentifier,
				.get_pathLenConstraint = _get_pathLenConstraint,
				.create_subjectAltName_enumerator = _create_subjectAltName_enumerator,
				.create_crl_uri_enumerator = _create_crl_uri_enumerator,
				.create_ocsp_uri_enumerator = _create_ocsp_uri_enumerator,
				.create_ipAddrBlock_enumerator = _create_ipAddrBlock_enumerator,
				.create_name_constraint_enumerator = _create_name_constraint_enumerator,
				.create_cert_policy_enumerator = _create_cert_policy_enumerator,
				.create_policy_mapping_enumerator = _create_policy_mapping_enumerator,
			},
		},
		.version = 1,
		.subjectAltNames = linked_list_create(),
		.crl_uris = linked_list_create(),
		.ocsp_uris = linked_list_create(),
		.ipAddrBlocks = linked_list_create(),
		.permitted_names = linked_list_create(),
		.excluded_names = linked_list_create(),
		.cert_policies = linked_list_create(),
		.policy_mappings = linked_list_create(),
		.pathLenConstraint = X509_NO_CONSTRAINT,
		.ref = 1,
	);
	return this;
}

/**
 * Build a generalName from an id
 */
chunk_t build_generalName(identification_t *id)
{
	int context;

	switch (id->get_type(id))
	{
		case ID_RFC822_ADDR:
			context = ASN1_CONTEXT_S_1;
			break;
		case ID_FQDN:
			context = ASN1_CONTEXT_S_2;
			break;
		case ID_DER_ASN1_DN:
			context = ASN1_CONTEXT_C_4;
			break;
		case ID_IPV4_ADDR:
		case ID_IPV6_ADDR:
			context = ASN1_CONTEXT_S_7;
			break;
		default:
			DBG1(DBG_LIB, "encoding %N as generalName not supported",
				 id_type_names, id->get_type(id));
			return chunk_empty;
	}
	return asn1_wrap(context, "c", id->get_encoding(id));
}

/**
 * Encode a linked list of subjectAltNames
 */
chunk_t x509_build_subjectAltNames(linked_list_t *list)
{
	chunk_t subjectAltNames = chunk_empty, name;
	enumerator_t *enumerator;
	identification_t *id;

	if (list->get_count(list) == 0)
	{
		return chunk_empty;
	}

	enumerator = list->create_enumerator(list);
	while (enumerator->enumerate(enumerator, &id))
	{
		name = build_generalName(id);
		subjectAltNames = chunk_cat("mm", subjectAltNames, name);
	}
	enumerator->destroy(enumerator);

	return asn1_wrap(ASN1_SEQUENCE, "mm",
						asn1_build_known_oid(OID_SUBJECT_ALT_NAME),
						asn1_wrap(ASN1_OCTET_STRING, "m",
							asn1_wrap(ASN1_SEQUENCE, "m", subjectAltNames)
						)
					 );
}

/**
 * Generate and sign a new certificate
 */
static bool generate(private_x509_cert_t *cert, certificate_t *sign_cert,
					 private_key_t *sign_key, int digest_alg)
{
	chunk_t extensions = chunk_empty, extendedKeyUsage = chunk_empty;
	chunk_t serverAuth = chunk_empty, clientAuth = chunk_empty;
	chunk_t ocspSigning = chunk_empty, certPolicies = chunk_empty;
	chunk_t basicConstraints = chunk_empty, nameConstraints = chunk_empty;
	chunk_t keyUsage = chunk_empty, keyUsageBits = chunk_empty;
	chunk_t subjectAltNames = chunk_empty, policyMappings = chunk_empty;
	chunk_t subjectKeyIdentifier = chunk_empty, authKeyIdentifier = chunk_empty;
	chunk_t crlDistributionPoints = chunk_empty, authorityInfoAccess = chunk_empty;
	identification_t *issuer, *subject;
	crl_uri_t *entry;
	chunk_t key_info;
	signature_scheme_t scheme;
	hasher_t *hasher;
	enumerator_t *enumerator, *uris;
	char *uri;

	subject = cert->subject;
	if (sign_cert)
	{
		issuer = sign_cert->get_subject(sign_cert);
		if (!cert->public_key)
		{
			return FALSE;
		}
	}
	else
	{	/* self signed */
		issuer = subject;
		if (!cert->public_key)
		{
			cert->public_key = sign_key->get_public_key(sign_key);
		}
		cert->flags |= X509_SELF_SIGNED;
	}
	cert->issuer = issuer->clone(issuer);
	if (!cert->notBefore)
	{
		cert->notBefore = time(NULL);
	}
	if (!cert->notAfter)
	{	/* defaults to 1 year from now */
		cert->notAfter = cert->notBefore + 60 * 60 * 24 * 365;
	}

	/* select signature scheme */
	cert->algorithm = hasher_signature_algorithm_to_oid(digest_alg,
								sign_key->get_type(sign_key));
	if (cert->algorithm == OID_UNKNOWN)
	{
		return FALSE;
	}
	scheme = signature_scheme_from_oid(cert->algorithm);

	if (!cert->public_key->get_encoding(cert->public_key,
										PUBKEY_SPKI_ASN1_DER, &key_info))
	{
		return FALSE;
	}

	/* encode subjectAltNames */
	subjectAltNames = x509_build_subjectAltNames(cert->subjectAltNames);

	/* encode CRL distribution points extension */
	enumerator = cert->crl_uris->create_enumerator(cert->crl_uris);
	while (enumerator->enumerate(enumerator, &entry))
	{
		chunk_t distributionPoint, gn;
		chunk_t crlIssuer = chunk_empty, gns = chunk_empty;

		if (entry->issuer)
		{
			crlIssuer = asn1_wrap(ASN1_CONTEXT_C_2, "m",
							build_generalName(entry->issuer));
		}
		uris = entry->uris->create_enumerator(entry->uris);
		while (uris->enumerate(uris, &uri))
		{
			gn = asn1_wrap(ASN1_CONTEXT_S_6, "c", chunk_create(uri, strlen(uri)));
			gns = chunk_cat("mm", gns, gn);
		}
		uris->destroy(uris);

		distributionPoint = asn1_wrap(ASN1_SEQUENCE, "mm",
								asn1_wrap(ASN1_CONTEXT_C_0, "m",
									asn1_wrap(ASN1_CONTEXT_C_0, "m", gns)),
								crlIssuer);
		crlDistributionPoints = chunk_cat("mm", crlDistributionPoints,
										  distributionPoint);
	}
	enumerator->destroy(enumerator);
	if (crlDistributionPoints.ptr)
	{
		crlDistributionPoints = asn1_wrap(ASN1_SEQUENCE, "mm",
					asn1_build_known_oid(OID_CRL_DISTRIBUTION_POINTS),
						asn1_wrap(ASN1_OCTET_STRING, "m",
							asn1_wrap(ASN1_SEQUENCE, "m", crlDistributionPoints)));
	}

	/* encode OCSP URIs in authorityInfoAccess extension */
	enumerator = cert->ocsp_uris->create_enumerator(cert->ocsp_uris);
	while (enumerator->enumerate(enumerator, &uri))
	{
		chunk_t accessDescription;

		accessDescription = asn1_wrap(ASN1_SEQUENCE, "mm",
								asn1_build_known_oid(OID_OCSP),
								asn1_wrap(ASN1_CONTEXT_S_6, "c",
										  chunk_create(uri, strlen(uri))));
		authorityInfoAccess = chunk_cat("mm", authorityInfoAccess,
										accessDescription);
	}
	enumerator->destroy(enumerator);
	if (authorityInfoAccess.ptr)
	{
		authorityInfoAccess = asn1_wrap(ASN1_SEQUENCE, "mm",
					asn1_build_known_oid(OID_AUTHORITY_INFO_ACCESS),
					asn1_wrap(ASN1_OCTET_STRING, "m",
						asn1_wrap(ASN1_SEQUENCE, "m", authorityInfoAccess)));
	}

	/* build CA basicConstraint and keyUsage flags for CA certificates */
	if (cert->flags & X509_CA)
	{
		chunk_t pathLenConstraint = chunk_empty;

		if (cert->pathLenConstraint != X509_NO_CONSTRAINT)
		{
			char pathlen = (char)cert->pathLenConstraint;

			pathLenConstraint = asn1_integer("c", chunk_from_thing(pathlen));
		}
		basicConstraints = asn1_wrap(ASN1_SEQUENCE, "mmm",
								asn1_build_known_oid(OID_BASIC_CONSTRAINTS),
								asn1_wrap(ASN1_BOOLEAN, "c",
									chunk_from_chars(0xFF)),
								asn1_wrap(ASN1_OCTET_STRING, "m",
										asn1_wrap(ASN1_SEQUENCE, "mm",
											asn1_wrap(ASN1_BOOLEAN, "c",
												chunk_from_chars(0xFF)),
											pathLenConstraint)));
		/* set CertificateSign and implicitly CRLsign */
		keyUsageBits = chunk_from_chars(0x01, 0x06);
	}
	else if (cert->flags & X509_CRL_SIGN)
	{
		keyUsageBits = chunk_from_chars(0x01, 0x02);
	}
	if (keyUsageBits.len)
	{
		keyUsage = asn1_wrap(ASN1_SEQUENCE, "mmm",
						asn1_build_known_oid(OID_KEY_USAGE),
						asn1_wrap(ASN1_BOOLEAN, "c", chunk_from_chars(0xFF)),
						asn1_wrap(ASN1_OCTET_STRING, "m",
							asn1_wrap(ASN1_BIT_STRING, "c", keyUsageBits)));
	}

	/* add serverAuth extendedKeyUsage flag */
	if (cert->flags & X509_SERVER_AUTH)
	{
		serverAuth = asn1_build_known_oid(OID_SERVER_AUTH);
	}
	if (cert->flags & X509_CLIENT_AUTH)
	{
		clientAuth = asn1_build_known_oid(OID_CLIENT_AUTH);
	}

	/* add ocspSigning extendedKeyUsage flag */
	if (cert->flags & X509_OCSP_SIGNER)
	{
		ocspSigning = asn1_build_known_oid(OID_OCSP_SIGNING);
	}

	if (serverAuth.ptr || clientAuth.ptr || ocspSigning.ptr)
	{
		extendedKeyUsage = asn1_wrap(ASN1_SEQUENCE, "mm",
								asn1_build_known_oid(OID_EXTENDED_KEY_USAGE),
								asn1_wrap(ASN1_OCTET_STRING, "m",
									asn1_wrap(ASN1_SEQUENCE, "mmm",
										serverAuth, clientAuth, ocspSigning)));
	}

	/* add subjectKeyIdentifier to CA and OCSP signer certificates */
	if (cert->flags & (X509_CA | X509_OCSP_SIGNER | X509_CRL_SIGN))
	{
		chunk_t keyid;

		if (cert->public_key->get_fingerprint(cert->public_key,
											  KEYID_PUBKEY_SHA1, &keyid))
		{
			subjectKeyIdentifier = asn1_wrap(ASN1_SEQUENCE, "mm",
									asn1_build_known_oid(OID_SUBJECT_KEY_ID),
									asn1_wrap(ASN1_OCTET_STRING, "m",
										asn1_wrap(ASN1_OCTET_STRING, "c", keyid)));
		}
	}

	/* add the keyid authKeyIdentifier for non self-signed certificates */
	if (sign_key)
	{
		chunk_t keyid;

		if (sign_key->get_fingerprint(sign_key, KEYID_PUBKEY_SHA1, &keyid))
		{
			authKeyIdentifier = asn1_wrap(ASN1_SEQUENCE, "mm",
							asn1_build_known_oid(OID_AUTHORITY_KEY_ID),
							asn1_wrap(ASN1_OCTET_STRING, "m",
								asn1_wrap(ASN1_SEQUENCE, "m",
									asn1_wrap(ASN1_CONTEXT_S_0, "c", keyid))));
		}
	}

	if (cert->permitted_names->get_count(cert->permitted_names) ||
		cert->excluded_names->get_count(cert->excluded_names))
	{
		chunk_t permitted = chunk_empty, excluded = chunk_empty, subtree;
		identification_t *id;

		enumerator = create_name_constraint_enumerator(cert, TRUE);
		while (enumerator->enumerate(enumerator, &id))
		{
			subtree = asn1_wrap(ASN1_SEQUENCE, "m", build_generalName(id));
			permitted = chunk_cat("mm", permitted, subtree);
		}
		enumerator->destroy(enumerator);
		if (permitted.ptr)
		{
			permitted = asn1_wrap(ASN1_CONTEXT_C_0, "m", permitted);
		}

		enumerator = create_name_constraint_enumerator(cert, FALSE);
		while (enumerator->enumerate(enumerator, &id))
		{
			subtree = asn1_wrap(ASN1_SEQUENCE, "m", build_generalName(id));
			excluded = chunk_cat("mm", excluded, subtree);
		}
		enumerator->destroy(enumerator);
		if (excluded.ptr)
		{
			excluded = asn1_wrap(ASN1_CONTEXT_C_1, "m", excluded);
		}

		nameConstraints = asn1_wrap(ASN1_SEQUENCE, "mm",
							asn1_build_known_oid(OID_NAME_CONSTRAINTS),
							asn1_wrap(ASN1_OCTET_STRING, "m",
								asn1_wrap(ASN1_SEQUENCE, "mm",
									permitted, excluded)));
	}

	if (cert->cert_policies->get_count(cert->cert_policies))
	{
		x509_cert_policy_t *policy;

		enumerator = create_cert_policy_enumerator(cert);
		while (enumerator->enumerate(enumerator, &policy))
		{
			chunk_t chunk = chunk_empty, cps = chunk_empty, notice = chunk_empty;

			if (policy->cps_uri)
			{
				cps = asn1_wrap(ASN1_SEQUENCE, "mm",
						asn1_build_known_oid(OID_POLICY_QUALIFIER_CPS),
						asn1_wrap(ASN1_IA5STRING, "c",
							chunk_create(policy->cps_uri,
										 strlen(policy->cps_uri))));
			}
			if (policy->unotice_text)
			{
				notice = asn1_wrap(ASN1_SEQUENCE, "mm",
							asn1_build_known_oid(OID_POLICY_QUALIFIER_UNOTICE),
							asn1_wrap(ASN1_SEQUENCE, "m",
								asn1_wrap(ASN1_VISIBLESTRING, "c",
									chunk_create(policy->unotice_text,
										strlen(policy->unotice_text)))));
			}
			if (cps.len || notice.len)
			{
				chunk = asn1_wrap(ASN1_SEQUENCE, "mm", cps, notice);
			}
			chunk = asn1_wrap(ASN1_SEQUENCE, "mm",
						asn1_wrap(ASN1_OID, "c", policy->oid), chunk);
			certPolicies = chunk_cat("mm", certPolicies, chunk);
		}
		enumerator->destroy(enumerator);

		certPolicies = asn1_wrap(ASN1_SEQUENCE, "mm",
							asn1_build_known_oid(OID_CERTIFICATE_POLICIES),
							asn1_wrap(ASN1_OCTET_STRING, "m",
								asn1_wrap(ASN1_SEQUENCE, "m", certPolicies)));
	}

	if (cert->policy_mappings->get_count(cert->policy_mappings))
	{
		x509_policy_mapping_t *mapping;

		enumerator = create_policy_mapping_enumerator(cert);
		while (enumerator->enumerate(enumerator, &mapping))
		{
			chunk_t chunk;

			chunk = asn1_wrap(ASN1_SEQUENCE, "mm",
						asn1_wrap(ASN1_OID, "c", mapping->issuer),
						asn1_wrap(ASN1_OID, "c", mapping->subject));
			policyMappings = chunk_cat("mm", policyMappings, chunk);
		}
		enumerator->destroy(enumerator);

		policyMappings = asn1_wrap(ASN1_SEQUENCE, "mm",
							asn1_build_known_oid(OID_POLICY_MAPPINGS),
							asn1_wrap(ASN1_OCTET_STRING, "m",
								asn1_wrap(ASN1_SEQUENCE, "m", policyMappings)));
	}

	if (basicConstraints.ptr || subjectAltNames.ptr || authKeyIdentifier.ptr ||
		crlDistributionPoints.ptr || nameConstraints.ptr)
	{
		extensions = asn1_wrap(ASN1_CONTEXT_C_3, "m",
						asn1_wrap(ASN1_SEQUENCE, "mmmmmmmmmmm",
							basicConstraints, keyUsage, subjectKeyIdentifier,
							authKeyIdentifier, subjectAltNames,
							extendedKeyUsage, crlDistributionPoints,
							authorityInfoAccess, nameConstraints, certPolicies,
							policyMappings));
	}

	cert->tbsCertificate = asn1_wrap(ASN1_SEQUENCE, "mmmcmcmm",
		asn1_simple_object(ASN1_CONTEXT_C_0, ASN1_INTEGER_2),
		asn1_integer("c", cert->serialNumber),
		asn1_algorithmIdentifier(cert->algorithm),
		issuer->get_encoding(issuer),
		asn1_wrap(ASN1_SEQUENCE, "mm",
			asn1_from_time(&cert->notBefore, ASN1_UTCTIME),
			asn1_from_time(&cert->notAfter, ASN1_UTCTIME)),
		subject->get_encoding(subject),
		key_info, extensions);

	if (!sign_key->sign(sign_key, scheme, cert->tbsCertificate, &cert->signature))
	{
		return FALSE;
	}
	cert->encoding = asn1_wrap(ASN1_SEQUENCE, "cmm", cert->tbsCertificate,
							   asn1_algorithmIdentifier(cert->algorithm),
							   asn1_bitstring("c", cert->signature));

	hasher = lib->crypto->create_hasher(lib->crypto, HASH_SHA1);
	if (!hasher)
	{
		return FALSE;
	}
	hasher->allocate_hash(hasher, cert->encoding, &cert->encoding_hash);
	hasher->destroy(hasher);
	return TRUE;
}

/**
 * See header.
 */
x509_cert_t *x509_cert_load(certificate_type_t type, va_list args)
{
	x509_flag_t flags = 0;
	chunk_t blob = chunk_empty;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_BLOB_ASN1_DER:
				blob = va_arg(args, chunk_t);
				continue;
			case BUILD_X509_FLAG:
				flags |= va_arg(args, x509_flag_t);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}

	if (blob.ptr)
	{
		private_x509_cert_t *cert = create_empty();

		cert->encoding = chunk_clone(blob);
		cert->parsed = TRUE;
		if (parse_certificate(cert))
		{
			cert->flags |= flags;
			return &cert->public;
		}
		destroy(cert);
	}
	return NULL;
}

/**
 * See header.
 */
x509_cert_t *x509_cert_gen(certificate_type_t type, va_list args)
{
	private_x509_cert_t *cert;
	certificate_t *sign_cert = NULL;
	private_key_t *sign_key = NULL;
	identification_t *crl_issuer = NULL;
	hash_algorithm_t digest_alg = HASH_SHA1;

	cert = create_empty();
	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_X509_FLAG:
				cert->flags |= va_arg(args, x509_flag_t);
				continue;
			case BUILD_SIGNING_KEY:
				sign_key = va_arg(args, private_key_t*);
				continue;
			case BUILD_SIGNING_CERT:
				sign_cert = va_arg(args, certificate_t*);
				continue;
			case BUILD_PUBLIC_KEY:
				cert->public_key = va_arg(args, public_key_t*);
				cert->public_key->get_ref(cert->public_key);
				continue;
			case BUILD_SUBJECT:
				cert->subject = va_arg(args, identification_t*);
				cert->subject = cert->subject->clone(cert->subject);
				continue;
			case BUILD_SUBJECT_ALTNAMES:
			{
				enumerator_t *enumerator;
				identification_t *id;
				linked_list_t *list;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &id))
				{
					cert->subjectAltNames->insert_last(cert->subjectAltNames,
													id->clone(id));
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_CRL_DISTRIBUTION_POINTS:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				crl_uri_t *entry;
				char *uri;

				list = va_arg(args, linked_list_t*);
				if (list->get_count(list))
				{
					entry = crl_uri_create(crl_issuer);
					enumerator = list->create_enumerator(list);
					while (enumerator->enumerate(enumerator, &uri))
					{
						entry->uris->insert_last(entry->uris, strdup(uri));
					}
					enumerator->destroy(enumerator);
					cert->crl_uris->insert_last(cert->crl_uris, entry);
				}
				continue;
			}
			case BUILD_CRL_ISSUER:
			{
				crl_issuer = va_arg(args, identification_t*);
				continue;
			}
			case BUILD_OCSP_ACCESS_LOCATIONS:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				char *uri;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &uri))
				{
					cert->ocsp_uris->insert_last(cert->ocsp_uris, strdup(uri));
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_PATHLEN:
				cert->pathLenConstraint = va_arg(args, int);
				if (cert->pathLenConstraint < 0 || cert->pathLenConstraint > 127)
				{
					cert->pathLenConstraint = X509_NO_CONSTRAINT;
				}
				continue;
			case BUILD_PERMITTED_NAME_CONSTRAINTS:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				identification_t *constraint;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &constraint))
				{
					cert->permitted_names->insert_last(cert->permitted_names,
												constraint->clone(constraint));
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_EXCLUDED_NAME_CONSTRAINTS:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				identification_t *constraint;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &constraint))
				{
					cert->excluded_names->insert_last(cert->excluded_names,
												constraint->clone(constraint));
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_CERTIFICATE_POLICIES:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				x509_cert_policy_t *policy, *in;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &in))
				{
					INIT(policy,
						.oid = chunk_clone(in->oid),
						.cps_uri = strdupnull(in->cps_uri),
						.unotice_text = strdupnull(in->unotice_text),
					);
					cert->cert_policies->insert_last(cert->cert_policies, policy);
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_POLICY_MAPPINGS:
			{
				enumerator_t *enumerator;
				linked_list_t *list;
				x509_policy_mapping_t* mapping, *in;

				list = va_arg(args, linked_list_t*);
				enumerator = list->create_enumerator(list);
				while (enumerator->enumerate(enumerator, &in))
				{
					INIT(mapping,
						.issuer = chunk_clone(in->issuer),
						.subject = chunk_clone(in->subject),
					);
					cert->policy_mappings->insert_last(cert->policy_mappings,
													   mapping);
				}
				enumerator->destroy(enumerator);
				continue;
			}
			case BUILD_NOT_BEFORE_TIME:
				cert->notBefore = va_arg(args, time_t);
				continue;
			case BUILD_NOT_AFTER_TIME:
				cert->notAfter = va_arg(args, time_t);
				continue;
			case BUILD_SERIAL:
				cert->serialNumber = chunk_clone(va_arg(args, chunk_t));
				continue;
			case BUILD_DIGEST_ALG:
				digest_alg = va_arg(args, int);
				continue;
			case BUILD_END:
				break;
			default:
				destroy(cert);
				return NULL;
		}
		break;
	}

	if (sign_key && generate(cert, sign_cert, sign_key, digest_alg))
	{
		return &cert->public;
	}
	destroy(cert);
	return NULL;
}


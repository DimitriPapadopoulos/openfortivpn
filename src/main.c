/*
 *  Copyright (c) 2015 Adrien Vergé
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "tunnel.h"
#include "userinput.h"
#include "log.h"
#include "http_server.h"

#include <openssl/ssl.h>

#include <unistd.h>
#include <getopt.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_USR_SBIN_PPPD && HAVE_USR_SBIN_PPP
#error "Both HAVE_USR_SBIN_PPPD and HAVE_USR_SBIN_PPP have been defined."
#elif HAVE_USR_SBIN_PPPD
#define PPPD_USAGE \
"                    [--pppd-use-peerdns=<0|1>] [--pppd-log=<file>]\n" \
"                    [--pppd-ifname=<string>] [--pppd-ipparam=<string>]\n" \
"                    [--pppd-call=<name>] [--pppd-plugin=<file>]\n" \
"                    [--pppd-accept-remote=<0|1>]\n"
#define PPPD_HELP \
"  --pppd-use-peerdns=[01]       Whether to ask peer ppp server for DNS server\n" \
"                                addresses and make pppd rewrite /etc/resolv.conf.\n" \
"  --pppd-no-peerdns             Same as --pppd-use-peerdns=0. pppd will not\n" \
"                                modify DNS resolution then.\n" \
"  --pppd-log=<file>             Set pppd in debug mode and save its logs into\n" \
"                                <file>.\n" \
"  --pppd-plugin=<file>          Use specified pppd plugin instead of configuring\n" \
"                                resolver and routes directly.\n" \
"  --pppd-ifname=<string>        Set the pppd interface name, if supported by pppd.\n" \
"  --pppd-ipparam=<string>       Provides an extra parameter to the ip-up, ip-pre-up\n" \
"                                and ip-down scripts. See man (8) pppd.\n" \
"  --pppd-call=<name>            Move most pppd options from pppd cmdline to\n" \
"                                /etc/ppp/peers/<name> and invoke pppd with\n" \
"                                'call <name>'.\n" \
"  --pppd-accept-remote=[01]     Whether to invoke pppd with 'ipcp-accept-remote'.\n" \
"                                Disable for pppd < 2.5.0.\n"
#elif HAVE_USR_SBIN_PPP
#define PPPD_USAGE \
"                    [--ppp-system=<system>]\n"
#define PPPD_HELP \
"  --ppp-system=<system>         Connect to the specified system as defined in\n" \
"                                /etc/ppp/ppp.conf.\n"
#else
#error "Neither HAVE_USR_SBIN_PPPD nor HAVE_USR_SBIN_PPP have been defined."
#endif

#if HAVE_RESOLVCONF
#define RESOLVCONF_USAGE \
"[--use-resolvconf=<0|1>] "
#define RESOLVCONF_HELP \
"  --use-resolvconf=[01]         If possible use resolvconf to update /etc/resolv.conf.\n"
#else
#define RESOLVCONF_USAGE ""
#define RESOLVCONF_HELP ""
#endif

#define usage \
"Usage: openfortivpn [<host>[:<port>]] [-u <user>] [-p <pass>]\n" \
"                    [--cookie=<cookie>] [--cookie-on-stdin] [--saml-login]\n" \
"                    [--otp=<otp>] [--otp-delay=<delay>] [--otp-prompt=<prompt>]\n" \
"                    [--pinentry=<program>] [--realm=<realm>]\n" \
"                    [--ifname=<ifname>] [--set-routes=<0|1>]\n" \
"                    [--half-internet-routes=<0|1>] [--set-dns=<0|1>]\n" \
PPPD_USAGE \
"                    " RESOLVCONF_USAGE "[--ca-file=<file>]\n" \
"                    [--user-cert=<file>] [--user-key=<file>]\n" \
"                    [--use-syslog] [--trusted-cert=<digest>]\n" \
"                    [--persistent=<interval>] [-c <file>] [-v|-q]\n" \
"       openfortivpn --help\n" \
"       openfortivpn --version\n" \
"\n"

#define summary \
"Client for PPP+TLS VPN tunnel services.\n" \
"openfortivpn connects to a VPN by setting up a tunnel to the gateway at\n" \
"<host>:<port>. It spawns a pppd process and operates the communication between\n" \
"the gateway and this process.\n" \
"\n"

#ifdef TLS1_3_VERSION
#define help_cipher_list " Applies to TLS v1.2 or\n" \
"                                lower only, not to be used with TLS v1.3 ciphers."
#define help_seclevel_1 " Applies to TLS v1.2 or lower only."
#else
#define help_cipher_list ""
#define help_seclevel_1 ""
#endif

#define help_options_part1 \
"Options:\n" \
"  -h --help                     Show this help message and exit.\n" \
"  --version                     Show version and exit.\n" \
"  -c <file>, --config=<file>    Specify a custom configuration file (default:\n" \
"                                " SYSCONFDIR "/openfortivpn/config).\n" \
"  -u <user>, --username=<user>  VPN account username.\n" \
"  -p <pass>, --password=<pass>  VPN account password.\n" \
"  --cookie=<cookie>             A valid session cookie (SVPNCOOKIE).\n" \
"  --cookie-on-stdin             Read the cookie (SVPNCOOKIE) from standard input.\n" \
"  --saml-login[=port]           Run a http server to handle SAML login requests\n" \
"  -o <otp>, --otp=<otp>         One-Time-Password.\n" \
"  --otp-prompt=<prompt>         Search for the OTP prompt starting with this string.\n" \
"  --otp-delay=<delay>           Wait <delay> seconds before sending the OTP.\n" \
"  --no-ftm-push                 Do not use FTM push if the server provides the option.\n" \
"  --pinentry=<program>          Use the program to supply a secret instead of asking for it.\n" \
"  --realm=<realm>               Use specified authentication realm.\n" \
"  --ifname=<interface>          Bind to interface.\n" \
"  --set-routes=[01]             Set if openfortivpn should configure routes\n" \
"                                when tunnel is up.\n"               \
"  --sni=<sni>                   Specify a different server name (SNI) than the host argument\n" \
"                                during TLS handshake.\n" \
"  --no-routes                   Do not configure routes, same as --set-routes=0.\n" \
"  --half-internet-routes=[01]   Add two 0.0.0.0/1 and 128.0.0.0/1 routes with higher\n" \
"                                priority instead of replacing the default route.\n" \
"  --set-dns=[01]                Set if openfortivpn should add DNS name servers\n" \
"                                and domain search list in /etc/resolv.conf.\n" \
"                                If installed resolvconf is used for the update.\n" \
"  --no-dns                      Do not reconfigure DNS, same as --set-dns=0.\n" \
"  --ca-file=<file>              Use specified PEM-encoded certificate bundle\n" \
"                                instead of system-wide store to verify the gateway\n" \
"                                certificate.\n" \
"  --user-cert=<file>            Use specified PEM-encoded certificate if the server\n" \
"                                requires authentication with a certificate.\n" \
"  --user-cert=pkcs11:           Use smartcard. Takes also partial or full PKCS11-URI.\n" \
"  --user-key=<file>             Use specified PEM-encoded key if the server requires\n" \
"                                authentication with a certificate.\n" \
"  --pem-passphrase=<pass>       Pass phrase for the PEM-encoded key.\n" \
"  --use-syslog                  Log to syslog instead of terminal.\n" \
"  --trusted-cert=<digest>       Trust a given gateway. If classical TLS\n" \
"                                certificate validation fails, the gateway\n" \
"                                certificate will be matched against this value.\n" \
"                                <digest> is the X509 certificate's sha256 sum.\n" \
"                                This option can be used multiple times to trust\n" \
"                                several certificates.\n"

#define help_options_part2 \
"  --insecure-ssl                Do not disable insecure TLS protocols/ciphers.\n" \
"                                Also enable TLS v1.0 if applicable.\n" \
"                                If your server requires a specific cipher or protocol,\n" \
"                                consider using --cipher-list and/or --min-tls instead.\n" \
"  --cipher-list=<ciphers>       OpenSSL ciphers to use. If default does not work\n" \
"                                you can try with the cipher suggested in the output\n" \
"                                of 'openssl s_client -connect <host:port>'\n" \
"                                (e.g. AES256-GCM-SHA384)." help_cipher_list "\n" \
"  --min-tls                     Use minimum TLS version instead of system default.\n" \
"                                Valid values are 1.0, 1.1, 1.2, 1.3.\n" \
"  --seclevel-1                  If --cipher-list is not specified, add @SECLEVEL=1 to\n" \
"                                (compiled in) list of ciphers. This lowers limits on\n" \
"                                dh key." help_seclevel_1 "\n" \
"  --persistent=<interval>       Run the vpn persistently in a loop and try to re-\n" \
"                                connect every <interval> seconds when dropping out.\n" \
"  -v                            Increase verbosity. Can be used multiple times\n" \
"                                to be even more verbose.\n" \
"  -q                            Decrease verbosity. Can be used multiple times\n" \
"                                to be even less verbose.\n"

#define help_config \
"\n" \
"Configuration file:\n" \
"  Options can be taken from a configuration file. Options passed in the\n" \
"  command line will override those from the configuration file, though. The\n" \
"  default configuration file is " SYSCONFDIR "/openfortivpn/config,\n" \
"  but this can be set using the -c option.\n" \
"  A simple configuration file example looks like:\n" \
"      # this is a comment\n" \
"      host = vpn-gateway\n" \
"      port = 8443\n" \
"      username = foo\n" \
"      password = bar\n" \
"      trusted-cert = certificatedigest4daa8c5fe6c...\n" \
"      trusted-cert = othercertificatedigest6631bf...\n" \
"  For a full-featured configuration see man openfortivpn(1).\n"

/**
 * Returns the given "input" prefixed with "prefix" in a dynamically
 * allocated string. This behaves exactly like "strdup" if "input" already
 * starts with "prefix".
 */
static char *strdup_with_prefix(const char *input, const char *prefix)
{
	size_t prefix_len = strlen(prefix);
	char *output;

	if (strncmp(prefix, input, prefix_len) == 0)
		return strdup(input);

	output = malloc(prefix_len + strlen(input) + 1);
	if (output) {
		strcpy(output, prefix);
		strcpy(output + prefix_len, input);
	}
	return output;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	const char *config_file = SYSCONFDIR "/openfortivpn/config";
	const char *host;
	char *port_str;

	struct vpn_config cfg = {
		.gateway_host = {'\0'},
		.gateway_port = 443,
		.username = {'\0'},
		.password = {'\0'},
		.password_set = 0,
		.cookie = NULL,
		.saml_port = 0,
		.saml_session_id = {'\0'},
		.otp = {'\0'},
		.otp_prompt = NULL,
		.otp_delay = 0,
		.no_ftm_push = 0,
		.pinentry = NULL,
		.realm = {'\0'},
		.iface_name = {'\0'},
		.sni = {'\0'},
		.set_routes = 1,
		.set_dns = 1,
		.use_syslog = 0,
		.half_internet_routes = 0,
		.persistent = 0,
#if HAVE_RESOLVCONF
		.use_resolvconf = USE_RESOLVCONF,
#endif
#if HAVE_USR_SBIN_PPPD
		.pppd_use_peerdns = 0,
		.pppd_log = NULL,
		.pppd_plugin = NULL,
		.pppd_ipparam = NULL,
		.pppd_ifname = NULL,
		.pppd_call = NULL,
#if LEGACY_PPPD
		.pppd_accept_remote = 0,
#else
		.pppd_accept_remote = 1,
#endif
#endif
#if HAVE_USR_SBIN_PPP
		.ppp_system = NULL,
#endif
		.ca_file = NULL,
		.user_cert = NULL,
		.user_key = NULL,
		.pem_passphrase = {'\0'},
		.pem_passphrase_set = 0,
		.insecure_ssl = 0,
#ifdef TLS1_2_VERSION
		.min_tls = TLS1_2_VERSION,
#else
		.min_tls = 0,
#endif
		.seclevel_1 = 0,
		.cipher_list = NULL,
		.cert_whitelist = NULL,
		.use_engine = 0,
		.user_agent = NULL,
	};
	struct vpn_config cli_cfg = invalid_cfg;

	const struct option long_options[] = {
		{"help",                 no_argument,       NULL, 'h'},
		{"version",              no_argument,       NULL, 0},
		{"config",               required_argument, NULL, 'c'},
		{"pinentry",             required_argument, NULL, 0},
		{"realm",                required_argument, NULL, 0},
		{"username",             required_argument, NULL, 'u'},
		{"password",             required_argument, NULL, 'p'},
		{"cookie",               required_argument, NULL, 0},
		{"cookie-on-stdin",      no_argument, NULL, 0},
		{"saml-login",           optional_argument, NULL, 0},
		{"otp",                  required_argument, NULL, 'o'},
		{"otp-prompt",           required_argument, NULL, 0},
		{"otp-delay",            required_argument, NULL, 0},
		{"no-ftm-push",          no_argument, &cli_cfg.no_ftm_push, 1},
		{"ifname",               required_argument, NULL, 0},
		{"set-routes",	         required_argument, NULL, 0},
		{"sni",                  required_argument, NULL, 0},
		{"no-routes",            no_argument, &cli_cfg.set_routes, 0},
		{"half-internet-routes", required_argument, NULL, 0},
		{"set-dns",              required_argument, NULL, 0},
		{"no-dns",               no_argument, &cli_cfg.set_dns, 0},
		{"use-syslog",           no_argument, &cli_cfg.use_syslog, 1},
		{"persistent",           required_argument, NULL, 0},
		{"ca-file",              required_argument, NULL, 0},
		{"user-cert",            required_argument, NULL, 0},
		{"user-key",             required_argument, NULL, 0},
		{"pem-passphrase",       required_argument, NULL, 0},
		{"trusted-cert",         required_argument, NULL, 0},
		{"insecure-ssl",         no_argument, &cli_cfg.insecure_ssl, 1},
		{"cipher-list",          required_argument, NULL, 0},
		{"min-tls",              required_argument, NULL, 0},
		{"seclevel-1",           no_argument, &cli_cfg.seclevel_1, 1},
#if HAVE_USR_SBIN_PPPD
		{"pppd-use-peerdns",     required_argument, NULL, 0},
		{"pppd-no-peerdns",      no_argument, &cli_cfg.pppd_use_peerdns, 0},
		{"pppd-log",             required_argument, NULL, 0},
		{"pppd-plugin",          required_argument, NULL, 0},
		{"pppd-ipparam",         required_argument, NULL, 0},
		{"pppd-ifname",          required_argument, NULL, 0},
		{"pppd-call",            required_argument, NULL, 0},
		{"pppd-accept-remote",   optional_argument, NULL, 0},
#endif
#if HAVE_USR_SBIN_PPP
		{"ppp-system",           required_argument, NULL, 0},
#endif
#if HAVE_RESOLVCONF
		{"use-resolvconf",       required_argument, NULL, 0},
#endif
		{NULL, 0, NULL, 0}
	};

	init_logging();

	while (1) {
		/* getopt_long stores the option index here. */
		int c, option_index = 0;

		c = getopt_long(argc, argv, "hvqc:u:p:o:",
		                long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
		case 0:
			/* If this option sets a flag, do nothing else now. */
			if (long_options[option_index].flag != 0)
				break;
			if (strcmp(long_options[option_index].name,
			           "version") == 0) {
				printf(VERSION "\n");
				if (strcmp(&REVISION[1], VERSION))
					log_debug("Revision " REVISION "\n");
				ret = EXIT_SUCCESS;
				goto exit;
			}
#if HAVE_USR_SBIN_PPPD
			if (strcmp(long_options[option_index].name,
			           "pppd-use-peerdns") == 0) {
				int pppd_use_peerdns = strtob(optarg);

				if (pppd_use_peerdns < 0) {
					log_warn("Bad pppd-use-peerdns option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.pppd_use_peerdns = pppd_use_peerdns;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-log") == 0) {
				free(cli_cfg.pppd_log);
				cli_cfg.pppd_log = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-plugin") == 0) {
				free(cli_cfg.pppd_plugin);
				cli_cfg.pppd_plugin = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-ifname") == 0) {
				free(cli_cfg.pppd_ifname);
				cli_cfg.pppd_ifname = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-ipparam") == 0) {
				free(cli_cfg.pppd_ipparam);
				cli_cfg.pppd_ipparam = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-call") == 0) {
				free(cli_cfg.pppd_call);
				cli_cfg.pppd_call = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pppd-accept-remote") == 0) {
				if (optarg) {
					int pppd_accept_remote = strtob(optarg);

					if (pppd_accept_remote < 0) {
						log_warn("Bad pppd-accept-remote option: \"%s\"\n",
						         optarg);
						break;
					}
					cli_cfg.pppd_accept_remote = pppd_accept_remote;
				} else {
					cli_cfg.pppd_accept_remote = 1;
				}
				break;
			}
#endif
#if HAVE_USR_SBIN_PPP
			if (strcmp(long_options[option_index].name,
			           "ppp-system") == 0) {
				free(cli_cfg.ppp_system);
				cfg.ppp_system = strdup(optarg);
				break;
			}
#endif
#if HAVE_RESOLVCONF
			if (strcmp(long_options[option_index].name,
			           "use-resolvconf") == 0) {
				int use_resolvconf = strtob(optarg);

				if (use_resolvconf < 0) {
					log_warn("Bad use-resolvconf option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.use_resolvconf = use_resolvconf;
				break;
			}
#endif
			if (strcmp(long_options[option_index].name,
			           "ca-file") == 0) {
				free(cli_cfg.ca_file);
				cli_cfg.ca_file = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "user-cert") == 0) {
				free(cli_cfg.user_cert);
				cli_cfg.user_cert = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "user-key") == 0) {
				free(cli_cfg.user_key);
				cli_cfg.user_key = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pem-passphrase") == 0) {
				strncpy(cli_cfg.pem_passphrase, optarg,
				        PEM_PASSPHRASE_SIZE);
				cli_cfg.pem_passphrase[PEM_PASSPHRASE_SIZE] = '\0';
				cli_cfg.pem_passphrase_set = 1;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "pinentry") == 0) {
				free(cli_cfg.pinentry);
				cli_cfg.pinentry = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "realm") == 0) {
				strncpy(cli_cfg.realm, optarg, REALM_SIZE);
				cli_cfg.realm[REALM_SIZE] = '\0';
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "trusted-cert") == 0) {
				if (add_trusted_cert(&cli_cfg, optarg))
					log_warn("Could not add certificate digest to whitelist.\n");
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "cipher-list") == 0) {
				free(cli_cfg.cipher_list);
				cli_cfg.cipher_list = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "min-tls") == 0) {
				int min_tls = parse_min_tls(optarg);

				if (min_tls == -1) {
					log_warn("Bad min-tls option: \"%s\"\n",
					         optarg);
				} else {
					cli_cfg.min_tls = min_tls;
				}
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "otp-prompt") == 0) {
				free(cli_cfg.otp_prompt);
				cli_cfg.otp_prompt = strdup(optarg);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "ifname") == 0) {
				strncpy(cli_cfg.iface_name, optarg, IF_NAMESIZE - 1);
				cli_cfg.iface_name[IF_NAMESIZE - 1] = '\0';
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "sni") == 0) {
				strncpy(cli_cfg.sni, optarg, GATEWAY_HOST_SIZE);
				cli_cfg.sni[GATEWAY_HOST_SIZE] = '\0';
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "set-routes") == 0) {
				int set_routes = strtob(optarg);

				if (set_routes < 0) {
					log_warn("Bad set-routes option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.set_routes = set_routes;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "half-internet-routes") == 0) {
				int half_internet_routes = strtob(optarg);

				if (half_internet_routes < 0) {
					log_warn("Bad half-internet-routes option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.half_internet_routes = half_internet_routes;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "otp-delay") == 0) {
				long otp_delay = strtol(optarg, NULL, 0);

				if (otp_delay < 0 || otp_delay > UINT_MAX) {
					log_warn("Bad otp-delay option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.otp_delay = otp_delay;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "persistent") == 0) {
				long persistent = strtol(optarg, NULL, 0);

				if (persistent < 0 || persistent > UINT_MAX) {
					log_warn("Bad persistent option: \"%s\"\n",
					         optarg);
					break;
				}
				cli_cfg.persistent = persistent;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "set-dns") == 0) {
				int set_dns = strtob(optarg);

				if (set_dns < 0) {
					log_warn("Bad set-dns option: \"%s\"\n", optarg);
					break;
				}
				cli_cfg.set_dns = set_dns;
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "cookie") == 0) {
				free(cli_cfg.cookie);
				cli_cfg.cookie = strdup_with_prefix(optarg, "SVPNCOOKIE=");
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "cookie-on-stdin") == 0) {
				char *cookie = read_from_stdin(COOKIE_SIZE);

				if (cookie == NULL) {
					log_error("Could not read the cookie from stdin\n");
					break;
				}
				free(cli_cfg.cookie);
				cli_cfg.cookie = strdup_with_prefix(cookie, "SVPNCOOKIE=");
				free(cookie);
				break;
			}
			if (strcmp(long_options[option_index].name,
			           "saml-login") == 0) {
				long port = 8020;

				if (optarg != NULL)
					port =	strtol(optarg, NULL, 0);

				if (port < 0 || port > 65535) {
					log_warn("Invalid saml listen port: %s! Default port is 8020 \n",
					         optarg);
					break;
				}
				cli_cfg.saml_port = port;
				break;
			}
			goto user_error;
		case 'h':
			printf("%s%s%s%s%s%s%s", usage, summary,
			       help_options_part1, help_options_part2,
			       PPPD_HELP, RESOLVCONF_HELP, help_config);
			ret = EXIT_SUCCESS;
			goto exit;
		case 'v':
			increase_verbosity();
			break;
		case 'q':
			decrease_verbosity();
			break;
		case 'c':
			config_file = optarg;
			break;
		case 'u':
			strncpy(cli_cfg.username, optarg, USERNAME_SIZE);
			cli_cfg.username[USERNAME_SIZE] = '\0';
			break;
		case 'p':
			strncpy(cli_cfg.password, optarg, PASSWORD_SIZE);
			cli_cfg.password[PASSWORD_SIZE] = '\0';
			cli_cfg.password_set = 1;
			while (*optarg)
				*optarg++ = '*';  // nuke it
			break;
		case 'o':
			strncpy(cli_cfg.otp, optarg, OTP_SIZE);
			cli_cfg.otp[OTP_SIZE] = '\0';
			break;
		default:
			goto user_error;
		}
	}

	if (optind < argc - 1 || optind > argc)
		goto user_error;

	if (cli_cfg.password[0] != '\0')
		log_warn("You should not pass the password on the command line. Type it interactively or use a configuration file instead.\n");

	log_debug_all("ATTENTION: the output contains sensitive information such as the THE CLEAR TEXT PASSWORD.\n");

	log_debug("openfortivpn " VERSION "\n");
	if (strcmp(&REVISION[1], VERSION))
		log_debug("revision " REVISION "\n");

	// Load configuration file
	if (config_file[0] != '\0') {
		ret = load_config(&cfg, config_file);
		if (ret == 0)
			log_debug("Loaded configuration file \"%s\".\n", config_file);
		else
			log_warn("Could not load configuration file \"%s\" (%s).\n",
			         config_file, err_cfg_str(ret));
	}
	if (cli_cfg.password_set) {
		if (cli_cfg.password[0] == '\0')
			log_debug("Disabled password due to empty command-line option\n");
	} else if (cfg.password_set) {
		if (cfg.password[0] == '\0')
			log_debug("Disabled password due to empty entry in configuration file \"%s\"\n",
			          config_file);
		else
			log_debug("Loaded password from configuration file \"%s\"\n",
			          config_file);
	}

	// Then apply CLI configuration
	merge_config(&cfg, &cli_cfg);
	set_syslog(cfg.use_syslog);

	// Set default UA
	if (cfg.user_agent == NULL)
		cfg.user_agent = strdup("Mozilla/5.0 SV1");

	// Read host and port from the command line
	if (optind == argc - 1) {
		host = argv[optind++];
		port_str = strchr(host, ':');
		if (port_str != NULL) {
			long port;

			port_str[0] = '\0';
			port_str++;
			port = strtol(port_str, NULL, 0);
			if (port < 1 || port > 65535) {
				log_error("Specify a valid port.\n");
				goto user_error;
			}
			cfg.gateway_port = (uint16_t)port;
		}
		strncpy(cfg.gateway_host, host, GATEWAY_HOST_SIZE);
		cfg.gateway_host[GATEWAY_HOST_SIZE] = '\0';
	}

	// Check host and port
	if (cfg.gateway_host[0] == '\0' || cfg.gateway_port == 0) {
		log_error("Specify a valid host:port couple.\n");
		goto user_error;
	}
	// Check username
	if (cfg.username[0] == '\0' && !cfg.cookie && !cfg.saml_port)
		// Need either username or cert
		if (cfg.user_cert == NULL) {
			log_error("Specify a username.\n");
			goto user_error;
		}
	// If username but no password given, interactively ask user
	if (!cfg.password_set && cfg.username[0] != '\0'
	    && !cfg.cookie && !cfg.saml_port) {
		char hint[USERNAME_SIZE + 1 + REALM_SIZE + 1 + GATEWAY_HOST_SIZE + 10];

		sprintf(hint, "%s_%s_%s_password",
		        cfg.username, cfg.realm, cfg.gateway_host);
		read_password(cfg.pinentry, hint,
		              "VPN account password: ",
		              cfg.password, PASSWORD_SIZE);
	}
	log_debug("Configuration host = \"%s\"\n", cfg.gateway_host);
	log_debug("Configuration realm = \"%s\"\n", cfg.realm);
	log_debug("Configuration port = \"%d\"\n", cfg.gateway_port);
	if (cfg.username[0] != '\0')
		log_debug("Configuration username = \"%s\"\n", cfg.username);
	log_debug_all("Configuration password = \"%s\"\n", cfg.password);
	if (cfg.otp[0] != '\0')
		log_debug("One-time password = \"%s\"\n", cfg.otp);

	if (geteuid() != 0) {
		log_error("This process was not spawned with root privileges, which are required.\n");
		ret = EXIT_FAILURE;
		goto exit;
	}

	if (cfg.saml_port != 0) {
		// Wait for the SAML token from the HTTP GET request
		if (wait_for_http_request(&cfg) != 0) {
			log_error("Failed to retrieve SAML cookie from HTTP\n");
			ret = EXIT_FAILURE;
			goto exit;
		}

		if (strlen(cfg.saml_session_id) == 0) {
			log_error("Failed to receive SAML session id\n");
			ret = EXIT_FAILURE;
			goto exit;
		}
	}

	do {
		if (run_tunnel(&cfg) != 0)
			ret = EXIT_FAILURE;
		else
			ret = EXIT_SUCCESS;
		if ((cfg.persistent > 0) && (get_sig_received() == 0))
			sleep(cfg.persistent);
	} while ((get_sig_received() == 0) && (cfg.persistent != 0));

	goto exit;

user_error:
	fprintf(stderr, "%s", usage);
exit:
	destroy_vpn_config(&cfg);
	exit(ret);
}

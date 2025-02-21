import json5
import re
import json
import ipaddress


CERTS_PATH, REST_PROTOCOL, REST_HOST, REST_PORT, REST_CSP, REST_CORS_ORIGINS = "", "", "", "", "", []


def validate_ip4(ip_str):
    try:
        # Create an IPv4 address object.
        ipaddress.IPv4Address(ip_str)
        return True
    except ipaddress.AddressValueError:
        return False


def validate_ip6(ip_str):
    try:
        # Create an IPv6 address object.
        ipaddress.IPv6Address(ip_str)
        return True
    except ipaddress.AddressValueError:
        return False


def validate_port(port):
    try:
        # Ports <= 1024 are reserved for system processes
        return 1024 <= port <= 65535
    except ValueError:
        return False


def set_config(options):
    if 'rest-port' not in options:
        return "`rest-port` option is not configured"
    global CERTS_PATH, REST_PROTOCOL, REST_HOST, REST_PORT, REST_CSP, REST_CORS_ORIGINS

    REST_PORT = int(options["rest-port"])
    if validate_port(REST_PORT) is False:
        return f"`rest-port` {REST_PORT}, should be a valid available port between 1024 and 65535."

    REST_HOST = str(options["rest-host"])
    if REST_HOST != "localhost" and validate_ip4(REST_HOST) is False and validate_ip6(REST_HOST) is False:
        return f"`rest-host` should be a valid IP."

    REST_PROTOCOL = str(options["rest-protocol"])
    if REST_PROTOCOL != "http" and REST_PROTOCOL != "https":
        return f"`rest-protocol` can either be http or https."

    CERTS_PATH = str(options["rest-certs"])
    REST_CSP = str(options["rest-csp"])
    cors_origins = options["rest-cors-origins"]
    REST_CORS_ORIGINS.clear()
    for origin in cors_origins:
        REST_CORS_ORIGINS.append(str(origin))

    return None


def call_rpc_method(plugin, rpc_method, payload):
    try:
        response = plugin.rpc.call(rpc_method, payload)
        if '"error":' in str(response).lower():
            raise Exception(response)
        else:
            plugin.log(f"{response}", "debug")
            if '"result":' in str(response).lower():
                # Use json5.loads ONLY when necessary, as it increases processing time
                return json.loads(response)["result"]
            else:
                return response

    except Exception as err:
        plugin.log(f"Error: {err}", "info")
        if "error" in str(err).lower():
            match_err_obj = re.search(r'"error":\{.*?\}', str(err))
            if match_err_obj is not None:
                err = "{" + match_err_obj.group() + "}"
            else:
                match_err_str = re.search(r"error: \{.*?\}", str(err))
                if match_err_str is not None:
                    err = "{" + match_err_str.group() + "}"
        raise Exception(err)


def verify_rune(plugin, rune, rpc_method, rpc_params):
    if rune is None:
        raise Exception('{ "error": {"code": 403, "message": "Not authorized: Missing rune"} }')

    return call_rpc_method(plugin, "checkrune",
                           {"rune": rune,
                            "method": rpc_method,
                            "params": rpc_params})


def process_help_response(help_response):
    # Use json5.loads due to single quotes in response
    processed_res = json5.loads(str(help_response))["help"]
    line = "\n---------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n\n"
    processed_html_res = ""
    for row in processed_res:
        processed_html_res += f"Command: {row['command']}\n"
        processed_html_res += f"Category: {row['category']}\n"
        processed_html_res += f"Description: {row['description']}\n"
        processed_html_res += f"Verbose: {row['verbose']}\n"
        processed_html_res += line
    return processed_html_res

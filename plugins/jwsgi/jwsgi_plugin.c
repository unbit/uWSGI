#include "../jvm/jvm.h"

extern struct uwsgi_server uwsgi;
extern uwsgi_jvm ujvm;

static int MAX_LREFS = 16;

void uwsgi_jwsgi_init(void) {

}

int uwsgi_jwsgi_request(struct wsgi_request *wsgi_req) {

    jmethodID jmid;
    int i;
    jobject env;
    jobject hkey, hval;
    jobject response;

    jobject status;
    jobject headers, header;
    jobject body;

    const char* body_str;
    const char* status_str;
    const char* hkey_str;
    const char* hval_str;

    jclass hc;

    jmethodID hh_size, hh_get;
    int hlen;

    if (!wsgi_req->uh.pktsize) {
        uwsgi_log("Invalid JWSGI request. skip.\n");
        return -1;
    }

    if (uwsgi_parse_vars(wsgi_req)) {
        uwsgi_log("Invalid JWSGI request. skip.\n");
        return -1;
    }

    uwsgi_jvm_begin(&ujvm, MAX_LREFS);

    uwsgi_log("main class = %s\n", ujvm.str_main);

    jmid = uwsgi_jvm_method_static(&ujvm, ujvm.class_main, "jwsgi", "(Ljava/util/Hashtable;)[Ljava/lang/Object;");

    uwsgi_log("jwsgi method id = %d\n", jmid);

    env = uwsgi_jvm_hashtable(&ujvm);
    uwsgi_jvm_default_handle(&ujvm);

    int cnt = wsgi_req->var_cnt;
    for(i=0;i<cnt;i++) {

        uwsgi_jvm_begin(&ujvm, MAX_LREFS);

        hkey = uwsgi_jvm_string_from(&ujvm, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len);
        hval = uwsgi_jvm_string_from(&ujvm, wsgi_req->hvec[i+1].iov_base, wsgi_req->hvec[i+1].iov_len);
        uwsgi_jvm_hashtable_put(&ujvm, env, hkey, hval);
        uwsgi_jvm_default_handle(&ujvm);

        uwsgi_jvm_end(&ujvm);

        i++;
    }

    uwsgi_log("env created\n");

    uwsgi_jvm_hashtable_put(&ujvm, env, uwsgi_jvm_utf8(&ujvm, "jwsgi.input"), uwsgi_jvm_fd(&ujvm, wsgi_req->poll.fd));

    uwsgi_log("jwsgi.input created\n");

    response = uwsgi_jvm_call(&ujvm, ujvm.class_main, jmid, env);
    uwsgi_jvm_default_handle(&ujvm);

    uwsgi_log("RESPONSE SIZE %d\n", uwsgi_jvm_arraylen(&ujvm, response));

    uwsgi_jvm_begin(&ujvm, MAX_LREFS);

    status = uwsgi_jvm_array_get(&ujvm, response, 0);
    uwsgi_jvm_default_handle(&ujvm);

    status_str = uwsgi_jvm_utf8chars(&ujvm, uwsgi_jvm_tostring(&ujvm, status));
    wsgi_req->headers_size += write(wsgi_req->poll.fd, wsgi_req->protocol, wsgi_req->protocol_len);
    wsgi_req->headers_size += write(wsgi_req->poll.fd, " ", 1);
    wsgi_req->headers_size += write(wsgi_req->poll.fd, status_str, uwsgi_jvm_utf8len(&ujvm, status));
    wsgi_req->headers_size += write(wsgi_req->poll.fd, "\r\n", 2);
    uwsgi_jvm_release_utf8chars(&ujvm, status, status_str);

    headers = uwsgi_jvm_array_get(&ujvm, response, 1);

    hc = uwsgi_jvm_class_of(&ujvm, headers);
    hh_size = uwsgi_jvm_method(&ujvm, hc, "size","()I");
    hh_get = uwsgi_jvm_method(&ujvm, hc, "get","(I)Ljava/lang/Object;");

    hlen = uwsgi_jvm_call_int(&ujvm, headers, hh_size);

    for(i=0;i<hlen;i++) {

        uwsgi_jvm_begin(&ujvm, MAX_LREFS);

        header = uwsgi_jvm_call(&ujvm, headers, hh_get, i);
        hkey = uwsgi_jvm_array_get(&ujvm, header, 0);
        hval = uwsgi_jvm_array_get(&ujvm, header, 1);
        hkey_str = uwsgi_jvm_utf8chars(&ujvm, hkey);
        hval_str = uwsgi_jvm_utf8chars(&ujvm, hval);

        wsgi_req->headers_size += write(wsgi_req->poll.fd, hkey_str, uwsgi_jvm_utf8len(&ujvm, hkey));
        wsgi_req->headers_size += write(wsgi_req->poll.fd, ": ", 2);
        wsgi_req->headers_size += write(wsgi_req->poll.fd, hval_str, uwsgi_jvm_utf8len(&ujvm, hval));
        wsgi_req->headers_size += write(wsgi_req->poll.fd, "\r\n", 2);

        uwsgi_jvm_release_utf8chars(&ujvm, hkey, hkey_str);
        uwsgi_jvm_release_utf8chars(&ujvm, hval, hval_str);

        uwsgi_jvm_end(&ujvm);
    }

    wsgi_req->headers_size += write(wsgi_req->poll.fd, "\r\n", 2);

    body = uwsgi_jvm_array_get(&ujvm, response, 2);
    body_str = uwsgi_jvm_utf8chars(&ujvm, body);
    wsgi_req->response_size = write(wsgi_req->poll.fd, body_str, uwsgi_jvm_utf8len(&ujvm, body));
    uwsgi_jvm_release_utf8chars(&ujvm, body, body_str);

    uwsgi_jvm_end(&ujvm);

    uwsgi_jvm_end(&ujvm);

    return 1;

}

void uwsgi_jwsgi_after_request(struct wsgi_request *wsgi_req) {

    log_request(wsgi_req);

}

struct uwsgi_plugin jwsgi_plugin = {

    .name = "jwsgi",
    .modifier1 = 8,
    .request = uwsgi_jwsgi_request,
    .after_request = uwsgi_jwsgi_after_request,

};

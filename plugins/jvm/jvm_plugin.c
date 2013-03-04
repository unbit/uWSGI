#include "jvm.h"

/*

with javap -s -p <class>
you can get method signatures

This plugin is the core for all of the JVM-based ones

some function (for performance reason) use static vars. They are thread safe in gcc.

*/

extern struct uwsgi_server uwsgi;
struct uwsgi_plugin jvm_plugin;
struct uwsgi_jvm ujvm;


int uwsgi_jvm_register_request_handler(uint8_t modifier2,int (*setup)(void), int (*func)(struct wsgi_request *)) {
	if (ujvm.request_handlers[modifier2] || ujvm.request_handlers_setup[modifier2]) {
		uwsgi_log("JVM request_handler for modifier2 %u already registered !!!\n", modifier2);
		return -1;
	}
	ujvm.request_handlers_setup[modifier2] = setup;
	ujvm.request_handlers[modifier2] = func;
	return 0;
}

JNIEXPORT jint JNICALL uwsgi_jvm_api_worker_id(JNIEnv *env, jclass c) {
	return uwsgi.mywid;
}

JNIEXPORT void JNICALL uwsgi_jvm_api_register_signal(JNIEnv *env, jclass c, jint signum, jstring target, jobject handler) {
	char *t = uwsgi_jvm_str2c(target);
	if (uwsgi_register_signal(signum, t, uwsgi_jvm_ref(handler), jvm_plugin.modifier1)) {
		uwsgi_jvm_throw("unable to register signal handler");
	}
}

JNIEXPORT void JNICALL uwsgi_jvm_api_register_rpc(JNIEnv *env, jclass c, jstring name, jobject func) {
	char *n = uwsgi_jvm_str2c(name);
	if (uwsgi_register_rpc(n, jvm_plugin.modifier1, 0, uwsgi_jvm_ref(func))) {
		uwsgi_jvm_throw("unable to register rpc function");
	}
}

static JNINativeMethod uwsgi_jvm_api_methods[] = {
	{"register_signal", "(ILjava/lang/String;Luwsgi$SignalHandler;)V", (void *) &uwsgi_jvm_api_register_signal},
	{"register_rpc", "(Ljava/lang/String;Luwsgi$RpcFunction;)V", (void *) &uwsgi_jvm_api_register_rpc},
	{"worker_id", "()I", (void *) &uwsgi_jvm_api_worker_id},
};

static struct uwsgi_option uwsgi_jvm_options[] = {
        {"jvm-main-class", required_argument, 0, "load the specified class and call its main() function", uwsgi_opt_add_string_list, &ujvm.main_classes, 0},
        {"jvm-class", required_argument, 0, "load the specified class", uwsgi_opt_add_string_list, &ujvm.classes, 0},
        {"jvm-classpath", required_argument, 0, "add the specified directory to the classpath", uwsgi_opt_add_string_list, &ujvm.classpath, 0},
        {0, 0, 0, 0},
};

// returns 0 if ok, -1 on exception
int uwsgi_jvm_exception(void) {

	if ((*ujvm_env)->ExceptionOccurred(ujvm_env)) {
        	(*ujvm_env)->ExceptionDescribe(ujvm_env);
        	(*ujvm_env)->ExceptionClear(ujvm_env);
		return -1;
	}
	return 0;
}

void uwsgi_jvm_clear_exception() {
	if ((*ujvm_env)->ExceptionOccurred(ujvm_env)) {
		(*ujvm_env)->ExceptionClear(ujvm_env);
	}
}

int uwsgi_jvm_object_is_instance(jobject o, jclass c) {
	if ((*ujvm_env)->IsInstanceOf(ujvm_env, o, c)) {
		return 1;
	}
	return 0;
}

// load/find/get a class
jclass uwsgi_jvm_class(char *name) {

	jclass my_class = (*ujvm_env)->FindClass(ujvm_env, name);

	if (uwsgi_jvm_exception()) {
		return NULL;
	}

	return my_class;
}

// get the class of an object
jclass uwsgi_jvm_class_from_object(jobject o) {
	return (*ujvm_env)->GetObjectClass(ujvm_env, o);	
}


// return a java string of the object class name
jobject uwsgi_jvm_object_class_name(jobject o) {
	jclass c = uwsgi_jvm_class_from_object(o);
	jmethodID mid = uwsgi_jvm_get_method_id(c, "getClass", "()Ljava/lang/Class;");
	if (!mid) return NULL;

	jobject oc = uwsgi_jvm_call_object(o, mid);
	if (!oc) return NULL;

	jclass c2 = uwsgi_jvm_class_from_object(oc);
        if (!c2) return NULL;	

	mid = uwsgi_jvm_get_method_id(c2, "getName", "()Ljava/lang/String;");
        if (!mid) return NULL;

	return uwsgi_jvm_call_object(oc, mid);
}

// returns the method id, given the method name and its signature
jmethodID uwsgi_jvm_get_method_id(jclass cls, char *name, char *signature) {
	jmethodID mid = (*ujvm_env)->GetMethodID(ujvm_env, cls, name, signature);
	if (uwsgi_jvm_exception()) {
                return NULL;
        }
        return mid;
}

// returns the static method id, given the method name and its signature
jmethodID uwsgi_jvm_get_static_method_id(jclass cls, char *name, char *signature) {
	jmethodID mid = (*ujvm_env)->GetStaticMethodID(ujvm_env, cls, name, signature);
	if (uwsgi_jvm_exception()) {
		return NULL;
	}
	return mid;
}

jobject uwsgi_jvm_ref(jobject obj) {
	return (*ujvm_env)->NewGlobalRef(ujvm_env, obj);
}

void uwsgi_jvm_unref(jobject obj) {
	(*ujvm_env)->DeleteLocalRef(ujvm_env, obj);
}

jobject uwsgi_jvm_hashmap() {
	// optimization
	static jmethodID mid = 0;

	if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.hashmap_class, "<init>", "()V");
		if (!mid) return NULL;
        }

	jobject hm = (*ujvm_env)->NewObject(ujvm_env, ujvm.hashmap_class, mid);
	if (uwsgi_jvm_exception()) {
		return NULL;
	}
	return hm;
}

int uwsgi_jvm_hashmap_put(jobject hm, jobject key, jobject value) {
	// optimization
        static jmethodID mid = 0;

        if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.hashmap_class, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
                if (!mid) return -1;
        }

	return uwsgi_jvm_call(hm, mid, key, value);
}

jobject uwsgi_jvm_hashmap_get(jobject hm, jobject key) {
        // optimization
        static jmethodID mid = 0;

        if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.hashmap_class, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
                if (!mid) return NULL;
        }

        return uwsgi_jvm_call_object(hm, mid, key);
}

jobject uwsgi_jvm_iterator(jobject set) {
	// optimization
        static jmethodID mid = 0;
	if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.set_class, "iterator", "()Ljava/util/Iterator;");
                if (!mid) return 0;
        }
	return uwsgi_jvm_call_object(set, mid);
}

int uwsgi_jvm_iterator_hasNext(jobject iterator) {
	// optimization
        static jmethodID mid = 0;

        if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.iterator_class, "hasNext", "()Z");
                if (!mid) return 0;
        }

        if (uwsgi_jvm_call_object(iterator, mid)) {
		return 1;
	}
	return 0;
}

jobject uwsgi_jvm_iterator_next(jobject iterator) {
        // optimization
        static jmethodID mid = 0;

        if (!mid) {
                mid = uwsgi_jvm_get_method_id(ujvm.iterator_class, "next", "()Ljava/lang/Object;");
                if (!mid) return NULL;
        }

        return uwsgi_jvm_call_object(iterator, mid);
}


jobject uwsgi_jvm_str(char *str, size_t len) {
	jobject new_str;
	if (len > 0) {
		char *tmp = uwsgi_concat2n(str, len, "", 0);
		new_str = (*ujvm_env)->NewStringUTF(ujvm_env, tmp);	
		free(tmp);
	}
	else {
		new_str = (*ujvm_env)->NewStringUTF(ujvm_env, str);
	}
	
	return new_str;
}

int uwsgi_jvm_call_static(jclass c, jmethodID mid, ...) {
	va_list args;
	va_start(args, mid);
	(*ujvm_env)->CallStaticVoidMethodV(ujvm_env, c, mid, args);
	va_end(args);
        return uwsgi_jvm_exception();
}

int uwsgi_jvm_call(jobject o, jmethodID mid, ...) {
	va_list args;
	va_start(args, mid);
        (*ujvm_env)->CallVoidMethodV(ujvm_env, o, mid, args);
	va_end(args);
        return uwsgi_jvm_exception();
}

jobject uwsgi_jvm_call_object_static(jclass c, jmethodID mid, ...) {
        va_list args;
        va_start(args, mid);
        jobject ret = (*ujvm_env)->CallStaticObjectMethodV(ujvm_env, c, mid, args);
        va_end(args);
        if (uwsgi_jvm_exception()) {
                return NULL;
        }
        return ret;
}


jobject uwsgi_jvm_call_object(jobject o, jmethodID mid, ...) {
	va_list args;
	va_start(args, mid);
        jobject ret = (*ujvm_env)->CallObjectMethodV(ujvm_env, o, mid, args);
	va_end(args);
        if (uwsgi_jvm_exception()) {
                return NULL;
        }
        return ret;
}


jobject uwsgi_jvm_call_objectA(jobject o, jmethodID mid, jvalue *args) {
	jobject ret = (*ujvm_env)->CallObjectMethodA(ujvm_env, o, mid, args);
	if (uwsgi_jvm_exception()) {
		return NULL;
	}	
	return ret;
}

void uwsgi_jvm_throw(char *message) {
	(*ujvm_env)->ThrowNew(ujvm_env, ujvm.runtime_exception, message);
}

static int uwsgi_jvm_init(void) {

	return 0;
}

static void uwsgi_jvm_create(void) {

	JavaVMOption options[1];

        ujvm.vm_args.version = JNI_VERSION_1_2;

        JNI_GetDefaultJavaVMInitArgs(&ujvm.vm_args);

        options[0].optionString = "-Djava.class.path=.";

        char *old_cp = NULL ;
        struct uwsgi_string_list *cp = ujvm.classpath;
        while(cp) {
                if (old_cp) {
                        options[0].optionString = uwsgi_concat3(old_cp, ":", cp->value);
                        free(old_cp);
                }
                else {
                        options[0].optionString = uwsgi_concat3(options[0].optionString, ":", cp->value);
                }
                old_cp = options[0].optionString ;
                cp = cp->next;
        }

        ujvm.vm_args.options  = options;
        ujvm.vm_args.nOptions = 1;

	JNIEnv  *env;
	if (pthread_key_create(&ujvm.env, NULL)) {
        	uwsgi_error("pthread_key_create()");
                exit(1);
        }

	if (JNI_CreateJavaVM(&ujvm.vm, (void **) &env, &ujvm.vm_args)) {
		uwsgi_log("unable to initialize the JVM\n");
		exit(1);
	}

	pthread_setspecific(ujvm.env, env);

	char *java_version = NULL;
	jvmtiEnv *jvmti;
	if ((*ujvm.vm)->GetEnv(ujvm.vm, (void **)&jvmti, JVMTI_VERSION) == JNI_OK) {
		(*jvmti)->GetSystemProperty(jvmti, "java.vm.version", &java_version);
	}

	if (uwsgi.mywid > 0) {
		if (java_version) {
			uwsgi_log("JVM %s initialized at %p (worker: %d pid: %d)\n", java_version, ujvm_env, uwsgi.mywid, (int) uwsgi.mypid);
		}
		else {
			uwsgi_log("JVM initialized at %p (worker: %d pid: %d)\n", ujvm_env, uwsgi.mywid, (int) uwsgi.mypid);
		}
	}

	ujvm.str_class = uwsgi_jvm_class("java/lang/String");
	if (!ujvm.str_class) exit(1);

	ujvm.hashmap_class = uwsgi_jvm_class("java/util/HashMap");
	if (!ujvm.hashmap_class) exit(1);

	ujvm.set_class = uwsgi_jvm_class("java/util/Set");
	if (!ujvm.set_class) exit(1);

	ujvm.iterator_class = uwsgi_jvm_class("java/util/Iterator");
	if (!ujvm.iterator_class) exit(1);

	ujvm.runtime_exception = uwsgi_jvm_class("java/lang/RuntimeException");
	if (!ujvm.runtime_exception) exit(1);

	jclass uwsgi_class = uwsgi_jvm_class("uwsgi");
	if (!uwsgi_class) {
		exit(1);
	}
	(*ujvm_env)->RegisterNatives(ujvm_env, uwsgi_class, uwsgi_jvm_api_methods, sizeof(uwsgi_jvm_api_methods)/sizeof(uwsgi_jvm_api_methods[0]));
	if (uwsgi_jvm_exception()) {
		exit(1);
	}

	jclass uwsgi_signal_handler_class = uwsgi_jvm_class("uwsgi$SignalHandler");
	if (!uwsgi_signal_handler_class) exit(1);
	ujvm.api_signal_handler_mid = uwsgi_jvm_get_method_id(uwsgi_signal_handler_class, "function", "(I)V");
	if (!ujvm.api_signal_handler_mid) exit(1);

	jclass uwsgi_rpc_function_class = uwsgi_jvm_class("uwsgi$RpcFunction");
	if (!uwsgi_rpc_function_class) exit(1);
	ujvm.api_rpc_function_mid = uwsgi_jvm_get_method_id(uwsgi_rpc_function_class, "function", "([Ljava/lang/String;)Ljava/lang/String;");
	if (!ujvm.api_rpc_function_mid) exit(1);

	struct uwsgi_string_list *usl = ujvm.main_classes;
	while(usl) {
		jclass c = uwsgi_jvm_class(usl->value);
		if (!c) {
			exit(1);
		}
		jmethodID mid = uwsgi_jvm_get_static_method_id(c, "main", "([Ljava/lang/String;)V");
		if (!mid) {
			mid = uwsgi_jvm_get_static_method_id(c, "main", "()V");
		}
		if (!mid) {
			uwsgi_log("unable to find main() method in class \"%s\"\n", usl->value);
			exit(1);
		}
		if (uwsgi_jvm_call_static(c, mid)) {
			exit(1);
		}
		usl = usl->next;
	}

	// load request_handlers setup functions
	int i;
	for(i=0;i<UMAX8;i++) {
		if (ujvm.request_handlers_setup[i]) {
			ujvm.request_handlers_setup[i]();
		}
	}

}

// get the raw body of a java string
char *uwsgi_jvm_str2c(jobject obj) {
    return (char *) (*ujvm_env)->GetStringUTFChars(ujvm_env, obj, NULL);
}

// return the size of a java string (UTF8)
size_t uwsgi_jvm_strlen(jobject obj) {
	return (*ujvm_env)->GetStringUTFLength(ujvm_env, obj);
}

static int uwsgi_jvm_signal_handler(uint8_t signum, void *handler) {
	long l_signum = signum;
	return uwsgi_jvm_call(handler, ujvm.api_signal_handler_mid, (void *) l_signum);
}

// route request to the specific JVM plugin (identified by modifier2)
static int uwsgi_jvm_request(struct wsgi_request *wsgi_req) {
	uint8_t modifier2 = wsgi_req->uh->modifier2;
	if (!ujvm.request_handlers[modifier2]) {
		uwsgi_log("unable to find JVM request handler %u\n", modifier2);
		return -1;
	}

	/* Standard JVM request */
        if (!wsgi_req->uh->pktsize) {
                uwsgi_log("Empty JVM request. skip.\n");
                return -1;
        }

        if (uwsgi_parse_vars(wsgi_req)) {
                return -1;
        }

	return ujvm.request_handlers[modifier2](wsgi_req);
}

void uwsgi_jvm_release_chars(jobject o, char *str) {
	(*ujvm_env)->ReleaseStringUTFChars(ujvm_env, o, str);
}

static uint16_t uwsgi_jvm_rpc(void *func, uint8_t argc, char **argv, uint16_t argvs[], char *buffer) {
	jvalue args[1];
	jobject str_array = (*ujvm_env)->NewObjectArray(ujvm_env, argc, ujvm.str_class, NULL);
	uint8_t i;
	for(i=0;i<argc;i++) {
		(*ujvm_env)->SetObjectArrayElement(ujvm_env, str_array, i, uwsgi_jvm_str(argv[i], argvs[i]));
	}
	args[0].l = str_array;
	jobject ret = uwsgi_jvm_call_objectA(func, ujvm.api_rpc_function_mid, args);
	(*ujvm_env)->DeleteLocalRef(ujvm_env, str_array);
	if (ret == NULL) {
		goto end;
	}
	size_t rlen = uwsgi_jvm_strlen(ret);
	if (rlen <= 0xffff) {
		char *b = uwsgi_jvm_str2c(ret);
		memcpy(buffer, b, rlen);
		uwsgi_jvm_release_chars(ret, b);
		(*ujvm_env)->DeleteLocalRef(ujvm_env, ret);
		return rlen;
	}
end:
	(*ujvm_env)->DeleteLocalRef(ujvm_env, ret);
	return 0;
}

static void uwsgi_jvm_init_thread(int coreid) {
	JNIEnv *env;
	(*ujvm.vm)->AttachCurrentThread(ujvm.vm, (void **) &env, NULL);
	pthread_setspecific(ujvm.env, env);
}

static void uwsgi_jvm_after_request(struct wsgi_request *wsgi_req) {
	log_request(wsgi_req);
}

struct uwsgi_plugin jvm_plugin = {
	.name = "jvm",
	.modifier1 = 8,

	.request = uwsgi_jvm_request,
	.after_request = uwsgi_jvm_after_request,

	.init = uwsgi_jvm_init,
	.options = uwsgi_jvm_options,

	.post_fork = uwsgi_jvm_create,

	.signal_handler = uwsgi_jvm_signal_handler,
	.rpc = uwsgi_jvm_rpc,

	.init_thread = uwsgi_jvm_init_thread,
};



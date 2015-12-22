#include <uwsgi.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#if LUA_VERSION_NUM < 502
# define lua_rawlen lua_objlen
#endif

extern struct uwsgi_server uwsgi;

struct uwsgi_lua {
	struct lua_State ***state;
	struct lua_State **mulestate;
	struct uwsgi_string_list *load;
	char *wsapi;
	struct uwsgi_string_list *postload;
	int gc_freq;
	uint8_t gc_perform;
	uint8_t shell;
	uint8_t shell_oneshot;
} ulua;

#define ULUA_MYWID (uwsgi.mywid-1)
#define ULUA_MYMID (uwsgi.muleid-1)
#define ULUA_WORKER_STATE ulua.state[ULUA_MYWID]
#define ULUA_MULE_STATE ulua.mulestate[ULUA_MYMID]
#define ULUA_LOG_HEADER "[uwsgi-lua]"

#define ULUA_MULE_MSG_GET_BUFFER_SIZE 65536

#define ULUA_MULE_MSG_REF 1

#define ULUA_WSAPI_REF 1
#define ULUA_SIGNAL_REF 2
#define ULUA_RPC_REF 3

#define ULUA_OPTDEF_GC_FREQ 1

#define ULUA_LOCK_LOCK 0
#define ULUA_LOCK_UNLOCK 1
#define ULUA_LOCK_CHECK 2

#define ulua_log(c, ar...) uwsgi_log(ULUA_LOG_HEADER" "c"\n", ##ar)

#define ULUA_WORKER_ANYAPP (ulua.wsapi ||\
	ulua.load ||\
	ulua.postload ||\
	ulua.shell ||\
	ulua.shell_oneshot)

struct uwsgi_plugin lua_plugin;

static void uwsgi_opt_luashell(char *opt, char *value, void *foobar) {
	uwsgi.honour_stdin = 1;
	ulua.shell = 1;
}

static void uwsgi_opt_luashell_oneshot(char *opt, char *value, void *foobar) {
	// enable shell
	uwsgi_opt_luashell(NULL, NULL, NULL);
	ulua.shell_oneshot = 1;
}

static struct uwsgi_option uwsgi_lua_options[] = {

	{"lua", required_argument, 0, "load lua wsapi app", uwsgi_opt_set_str, &ulua.wsapi, 0},
	{"lua-load", required_argument, 0, "load a lua file before wsapi app", uwsgi_opt_add_string_list, &ulua.load, 0},
	{"lua-postload", required_argument, 0, "load a lua file after wsapi app", uwsgi_opt_add_string_list, &ulua.postload, 0},
	{"lua-shell", no_argument, 0, "run the lua interactive shell (debug.debug())", uwsgi_opt_luashell, NULL, 0},
	{"luashell", no_argument, 0, "run the lua interactive shell (debug.debug())", uwsgi_opt_luashell, NULL, 0},
	{"lua-shell-oneshot", no_argument, 0, "run the lua interactive shell (debug.debug(), one-shot variant)", uwsgi_opt_luashell_oneshot, NULL, 0},
	{"luashell-oneshot", no_argument, 0, "run the lua interactive shell (debug.debug(), one-shot variant)", uwsgi_opt_luashell_oneshot, NULL, 0},
	{"lua-gc-freq", required_argument, 0, "set the lua gc frequency (default: 1, runs after every request)", uwsgi_opt_set_int, &ulua.gc_freq, 0},
	{"lua-gc-full", no_argument, 0, "set the lua gc to perform a full garbage-collection cycle (default: 0, gc performs an incremental step of garbage collection)", uwsgi_opt_set_int, &ulua.gc_perform, 0},

	{0, 0, 0, 0},

};

static int uwsgi_lua_isutable(lua_State *L, int obj) {

	int type = lua_type(L, obj);
	return (type == LUA_TTABLE || type == LUA_TUSERDATA);
}

static int uwsgi_lua_metatable_tostring(lua_State *L, int obj) {

	if (!(luaL_callmeta(L, obj, "__tostring"))) {
		return 0;
	}

	if (lua_isstring(L, -1)) {
		lua_replace(L, obj-1);
		return 1;
	}

	ulua_log("warning: __tostring must return a string value");
	lua_pop(L, 1);
	return 0;
}

static int uwsgi_lua_metatable_tostring_protected(lua_State *L, int obj) {
	// replace table with __tostring result, or do nothing in case of fail

	if (!(luaL_getmetafield(L, obj, "__tostring"))) {
		return 0;
	}

	lua_pushvalue(L, --obj);

	if (lua_pcall(L, 1, 1, 0)) {
		ulua_log("%s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1; // runtime error
	}

	if (lua_isstring(L, -1)) {
		lua_replace(L, obj);
		return 1;
	}

	ulua_log("warning: __tostring must return a string value");
	lua_pop(L, 1);
	return 0;
}


static int uwsgi_lua_metatable_call(lua_State *L, int obj) {
	// get __call attr and place it before table, or do nothing in case of fail

	if (!(luaL_getmetafield(L, obj, "__call"))) {
		return 0;
	}

	if (!(lua_isfunction(L, -1))) {
		ulua_log("__call is not a function");
		lua_pop(L, 1);
		return 0;
	}

	lua_insert(L, obj-1);

	return 1;
}

static int uwsgi_lua_killsig(lua_State *L, uint8_t signal) {

	if (kill(uwsgi.workers[0].pid, signal)) {
		uwsgi_error("kill()");
		lua_pushnil(L);
	} else {
		lua_pushboolean(L, 1);
	}

	return 1;
}

static int uwsgi_api_reload(lua_State *L) {
	return uwsgi_lua_killsig(L, SIGHUP);
}

static int uwsgi_api_stop(lua_State *L) {
	return uwsgi_lua_killsig(L, SIGQUIT);
}

static int uwsgi_api_micros(lua_State *L) {

	lua_pushnumber(L, uwsgi_micros());
	return 1;
}

static int uwsgi_api_request_id(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	lua_pushnumber(L, uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].requests);
	return 1;
}

static int uwsgi_api_metric_get(lua_State *L) {

	if (!(lua_gettop(L)) || !(lua_isstring(L, 1))) {
		return 0;
	}

	lua_pushnumber(L, uwsgi_metric_get((char *) lua_tostring(L, 1), NULL));

	return 1;
}

static int uwsgi_api_metric_set(lua_State *L) {

	if ((lua_gettop(L) < 2) ||
		!(lua_isstring(L, 1)) ||
		!(lua_isnumber(L, 2)) ||
		uwsgi_metric_set((char *) lua_tostring(L, 1), NULL, lua_tonumber(L, 2)))
	{
		return 0;
	}

	lua_pushboolean(L, 1);

	return 1;
}

static int uwsgi_lua_metric_do(lua_State *L, int metric_do(char*, char*, int64_t)) {
	int64_t value = 1;
	uint16_t argc = lua_gettop(L);

	if (!(argc) || !(lua_isstring(L, 1))) {
		return 0;
	}

	if (argc > 1 && lua_isnumber(L, 2)) {
		value = lua_tonumber(L, 2);
	}

	if (metric_do((char *) lua_tostring(L, 1), NULL, value)) {
		return 0;
	}

	lua_pushboolean(L, 1);

	return 1;
}

static int uwsgi_api_metric_inc(lua_State *L) {
	return uwsgi_lua_metric_do(L, uwsgi_metric_inc);
}

static int uwsgi_api_metric_div(lua_State *L) {
	return uwsgi_lua_metric_do(L, uwsgi_metric_div);
}

static int uwsgi_api_metric_mul(lua_State *L) {
	return uwsgi_lua_metric_do(L, uwsgi_metric_mul);
}

static int uwsgi_api_metric_dec(lua_State *L) {
	return uwsgi_lua_metric_do(L, uwsgi_metric_dec);
}


static int uwsgi_api_signal(lua_State *L) {
	uint16_t argc = lua_gettop(L);

	if (argc > 0 && lua_isnumber(L, 1)) {
		if (argc > 1 && lua_isstring(L, 2)) {
			lua_pushnumber(L, uwsgi_remote_signal_send((char *) lua_tostring(L, -2), (uint8_t) lua_tonumber(L, 1)));

			return 1;
		} else {
			uwsgi_signal_send(uwsgi.signal_socket, (uint8_t) lua_tonumber(L, 1));
		}
	}

	return 0;
}

static const char *uwsgi_lua_log_tostring(lua_State *L, int obj, size_t *len) {

	int type = lua_type(L, obj);
	const void *point;

	switch(type) {
		case LUA_TNIL: if (len) *len = 3; return "nil";
		case LUA_TNONE: if (len) *len = 4; return "none";
		case LUA_TBOOLEAN:

			if (lua_toboolean(L, obj)) {
				if (len) *len = 4;
				return "true";
			}

			if (len) *len = 5;
			return "false";

		case LUA_TSTRING:
		case LUA_TNUMBER: break;
		case LUA_TUSERDATA:
		case LUA_TTABLE: if (uwsgi_lua_metatable_tostring(L, obj)) break;
		default:

			point = lua_topointer(L, obj);

			if (point) {
				lua_pushfstring(L, "<%s: %p>", lua_typename(L, type), point);
			} else {
				lua_pushfstring(L, "<%s>", lua_typename(L, type));
			}

			lua_replace(L, obj-1);
	}

	return lua_tolstring(L, obj, len);
}

static int uwsgi_api_log(lua_State *L) {

	struct uwsgi_buffer *ub;
	uint16_t argc = lua_gettop(L);

	const char *str;
	size_t len;
	int i;

	if (argc > 1) {
		ub = uwsgi_buffer_new(0);

		for (i = 1 - argc; i < 0; i++) {
			str = uwsgi_lua_log_tostring(L, i, &len);
			if (uwsgi_buffer_ensure(ub, len + 2)) break;
			if (uwsgi_buffer_byte(ub, ' ')) break;
			if (uwsgi_buffer_append(ub, (char *) str, len)) break;
		}

		if (!uwsgi_buffer_byte(ub, 0))
			ulua_log("%s%s", uwsgi_lua_log_tostring(L, -argc, NULL), ub->buf);
		uwsgi_buffer_destroy(ub);

	} else if (argc) {

		ulua_log("%s", uwsgi_lua_log_tostring(L, -argc, NULL));
	}

	return 0;
}

static int uwsgi_api_rpc(lua_State *L) {

	uint16_t argc = lua_gettop(L);
	uint8_t argnum;
	uint8_t i;
	uint64_t len;

	if (argc < 2) {
		return 0;
	}

	// take first 256
	argnum = (argc <= 256) ? (argc - 2) : 255;

	char **argv = NULL;
	uint16_t *argvs = NULL;

	if (argnum > 0) {

		argv = (char **) uwsgi_malloc(sizeof(char *)*argnum);
		argvs = (uint16_t *) uwsgi_malloc(sizeof(uint16_t)*argnum);

		for(i = 0; i < argnum; i++) {
			if (uwsgi_lua_isutable(L, i + 3)) {
				uwsgi_lua_metatable_tostring(L, i + 2 - argc);
			}

			argv[i] = (char *) lua_tolstring(L, i + 3, (size_t *) &argvs[i]);
		}
	}

	char *str = uwsgi_do_rpc((char *) lua_tostring(L, 1), (char *) lua_tostring(L, 2), argnum, argv, argvs, &len);

	if (!(len)) { // fail??
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, str, len);
	}

	if (argnum > 0) {
		free(argv);
		free(argvs);
	}

	free(str);

	return 1;
}

static int uwsgi_api_rpc_register_key(const char *key, size_t len) {

	// exists??
	size_t i;
	int offset = uwsgi.mywid * uwsgi.rpc_max;

	for(i = 0; i < uwsgi.shared->rpc_count[uwsgi.mywid]; i++) {
		if (!strcmp(key, (&uwsgi.rpc_table[offset + i])->name)) {
			if ((&uwsgi.rpc_table[offset + i])->plugin == &lua_plugin) {
				return 1;
			}

			break;
		}
	}

	// no
	char *name = (char *) uwsgi_malloc(sizeof(char) * len);

	memcpy(name, key, sizeof(char) * len);

	if (uwsgi_register_rpc(name, &lua_plugin, 0, name)) {
		// error
		free(name);
		return 0;
	}

	return 1;
}

static int uwsgi_api_register_rpc_newindex(lua_State *L) {
	// 3 args: table, key(string or number), value(not nil)

	uint16_t argc = lua_gettop(L);
	size_t len;

	if (argc != 3) {
		return 0;
	}

	const char *key = lua_tolstring(L, -2, &len);

	if (len && !(lua_isnil(L, -1)) && uwsgi_api_rpc_register_key(key, len + 1)) {
		lua_rawset(L, -3);
	}

	return 0;
}

static int uwsgi_api_register_rpc(lua_State *L) {
	// legacy rpc register func

	uint16_t argc = lua_gettop(L);

	if (argc < 2) {
		return 0;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_RPC_REF);
	lua_pushcfunction(L, uwsgi_api_register_rpc_newindex);

	lua_pushvalue(L, -2);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);

	lua_call(L, 3, 0);

	lua_pushvalue(L, 1);
	lua_rawget(L, -2);

	if (!lua_isnil(L, -1)) {
		lua_pushboolean(L, 1);
	}

	return 1;
}


static int uwsgi_lua_cache_magic_set_value(lua_State *L, uint16_t argc, uint8_t isnum, uint64_t flag) {

	char *cache = NULL;
	uint64_t expires = 0;

	char *key;
	size_t keylen;

	char *value;
	size_t valuelen;
	int64_t valuenum;

	if (argc > 2) {
		expires = lua_tonumber(L, 3);
		if (argc > 3) {
			cache = (char *) lua_tostring(L, 4);
		}
	}

	// key
	key = (char *) lua_tolstring(L, 1, &keylen);

	// value
	if (uwsgi_lua_isutable(L, 2)) {
		uwsgi_lua_metatable_tostring(L, -argc + 1);
	}

	if (isnum) {
		if (lua_isnumber(L, 2)) {
			valuenum = lua_tonumber(L, 2);
			if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) return 0;
		} else if (flag & UWSGI_CACHE_FLAG_MATH) {
			valuenum = 1;
			if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) return 0;
		}
	} else {
		value = (char *) lua_tolstring(L, 2, &valuelen);
		if (uwsgi_cache_magic_set(key, keylen, value, valuelen, expires, flag, cache)) return 0;
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int uwsgi_lua_cache_magic_set_table(lua_State *L, uint16_t argc, uint8_t isnum, uint64_t flag) {

	size_t error = 0;

	char *cache = NULL;
	uint64_t expires = 0;

	char *key;
	size_t keylen;

	char *value;
	size_t valuelen;
	int64_t valuenum;

	if (argc > 1) {
		expires = lua_tonumber(L, 2);
		if (argc > 2) {
			cache = (char *) lua_tostring(L, 3);
		}
	}

	lua_pushnil(L);

	while(lua_next(L, 1)) {
		lua_pushvalue(L, -2);

		//key
		key = (char *) lua_tolstring(L, -1, &keylen);

		//value
		if (uwsgi_lua_isutable(L, -2)) {
			uwsgi_lua_metatable_tostring(L, -2);
		}

		if (isnum) {
			if (lua_isnumber(L, -2)) {
				valuenum = lua_tonumber(L, -2);
				if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) ++error;
			} else if (flag & UWSGI_CACHE_FLAG_MATH) {
				valuenum = 1;
				if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) ++error;
			} else {
				++error;
			}
		} else {
			value = (char *) lua_tolstring(L, -2, &valuelen);
			if (uwsgi_cache_magic_set(key, keylen, value, valuelen, expires, flag, cache)) ++error;
		}

		lua_pop(L, 2);
	}

	if (!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;

}

static int uwsgi_lua_cache_magic_set(lua_State *L, uint8_t isnum, uint64_t flag) {
	uint16_t argc = lua_gettop(L);

	if (argc > 0 && lua_istable(L, 1)) {
		return uwsgi_lua_cache_magic_set_table(L, argc, isnum, flag);
	} else if (argc > 1 || (flag & UWSGI_CACHE_FLAG_MATH && argc)) {
		return uwsgi_lua_cache_magic_set_value(L, argc, isnum, flag);
	}

	return 0;
}

static int uwsgi_api_cache_set(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 0, 0);
}

static int uwsgi_api_cache_setnum(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, 0);
}

static int uwsgi_api_cache_update(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 0, UWSGI_CACHE_FLAG_UPDATE);
}

static int uwsgi_api_cache_updatenum(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, UWSGI_CACHE_FLAG_UPDATE);
}

static int uwsgi_api_cache_inc(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_INC);
}

static int uwsgi_api_cache_dec(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_DEC);
}

static int uwsgi_api_cache_mul(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_MUL);
}

static int uwsgi_api_cache_div(lua_State *L) {
	return uwsgi_lua_cache_magic_set(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_DIV);
}

static int uwsgi_lua_cache_magic_set_multi(lua_State *L, uint8_t isnum, uint64_t flag) {

	uint16_t argc = lua_gettop(L);
	uint16_t error = 0;
	uint16_t i;

	char *cache;
	uint64_t expires;

	char *key;
	size_t keylen;

	char *value;
	size_t valuelen;
	int64_t valuenum;

	if (argc < 3) {
		return 0;
	}

	expires = lua_tonumber(L, 1);
	cache = (char *) lua_tostring(L, 2);

	for (i = 4, ++argc; i <= argc; i+=2) {
		// key
		key = (char *) lua_tolstring(L, i - 1, &keylen);

		// value
		if (uwsgi_lua_isutable(L, i)) {
			uwsgi_lua_metatable_tostring(L, i - argc - 1);
		}

		if (isnum) {
			if (lua_isnumber(L, i)) {
				valuenum = lua_tonumber(L, i);
				if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) ++error;
			} else if (flag & UWSGI_CACHE_FLAG_MATH) {
				valuenum = 1;
				if (uwsgi_cache_magic_set(key, keylen, (char *) &valuenum, 8, expires, flag, cache)) ++error;
			} else {
				++error;
			}
		} else {
			value = (char *) lua_tolstring(L, i, &valuelen);
			if (uwsgi_cache_magic_set(key, keylen, value, valuelen, expires, flag, cache)) ++error;
		}
	}

	if(!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;
}

static int uwsgi_api_cache_set_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 0, 0);
}

static int uwsgi_api_cache_setnum_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, 0);
}

static int uwsgi_api_cache_update_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 0, UWSGI_CACHE_FLAG_UPDATE);
}

static int uwsgi_api_cache_updatenum_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, UWSGI_CACHE_FLAG_UPDATE);
}

static int uwsgi_api_cache_inc_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_INC);
}

static int uwsgi_api_cache_dec_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_DEC);
}

static int uwsgi_api_cache_mul_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_MUL);
}

static int uwsgi_api_cache_div_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_set_multi(L, 1, UWSGI_CACHE_FLAG_UPDATE|UWSGI_CACHE_FLAG_MATH|UWSGI_CACHE_FLAG_FIXEXPIRE|UWSGI_CACHE_FLAG_DIV);
}


static int uwsgi_api_cache_clear(lua_State *L) {

	char *cache = NULL;

	if (lua_gettop(L)) {
		cache = (char *) lua_tostring(L, 1);
	}

	if (!uwsgi_cache_magic_clear(cache)) {
		lua_pushboolean(L, 1);
		return 1;
	}

	return 0;
}

static int uwsgi_api_cache_keys(lua_State *L) {

	char *cache = NULL;

	if (lua_gettop(L)) {
		cache = (char *) lua_tostring(L, 1);
	}

	struct uwsgi_cache *uc = uwsgi_cache_by_name(cache);
	struct uwsgi_cache_item *uci = NULL;
	uint64_t pos = 0;
	uint64_t i = 0;

	if (!uc) {
		ulua_log("uWSGI cache '%s' is not available!", cache != NULL ? cache : "default");
		return 0;
	}

	lua_newtable(L);

	uwsgi_rlock(uc->lock);
	do {
		uci = uwsgi_cache_keys(uc, &pos, &uci);

		if (uci) {
			lua_pushlstring(L, uci->key, uci->keysize);
			lua_rawseti(L, -2, ++i);
		}

	} while (uci);
	uwsgi_rwunlock(uc->lock);

	return 1;
}


static int uwsgi_lua_cache_del_exists(lua_State *L, int cache_do(char *, uint16_t, char *), uint8_t reverse) {

	size_t keylen;
	char *key;
	char *cache = NULL;

	size_t i, tlen;
	size_t error = 0;

	uint16_t argc = lua_gettop(L);

	if (!(argc)) {
		return 0;
	}

	if (argc > 1) {
		cache = (char *) lua_tostring(L, 2);
	}

	// get the key
	if (lua_istable(L, 1)) {
		tlen = lua_rawlen(L, 1);

		for(i = 1; i <= tlen; i++) {
			lua_rawgeti(L, 1, i);

			key = (char *) lua_tolstring(L, -1, &keylen);

			if (!keylen || (cache_do(key, keylen, cache) - reverse)) {
				++error;
			}

			lua_pop(L, 1);
		}

	} else {
		key = (char *) lua_tolstring(L, 1, &keylen);

		if (!keylen || (cache_do(key, keylen, cache) - reverse)) {
			lua_pushnil(L);
			return 1;
		}
	}

	if (!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;
}

static int uwsgi_api_cache_del(lua_State *L) {
	return uwsgi_lua_cache_del_exists(L, uwsgi_cache_magic_del, 0);
}

static int uwsgi_api_cache_exists(lua_State *L) {
	return uwsgi_lua_cache_del_exists(L, uwsgi_cache_magic_exists, 1);
}

static int uwsgi_api_cache_del_multi(lua_State *L) {

	size_t keylen;
	char *key;
	char *cache;

	uint16_t argc = lua_gettop(L);
	uint16_t error = 0;
	uint16_t i;

	if (argc < 1) {
		return 0;
	}

	cache = (char *) lua_tostring(L, 1);

	for (i = 2; i <= argc; i++) {
		key = (char *) lua_tolstring(L, i, &keylen);

		if (!keylen || uwsgi_cache_magic_del(key, keylen, cache)) {
			++error;
		}
	}

	if (!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;

}

static int uwsgi_api_cache_exists_multi(lua_State *L) {

	size_t keylen;
	char *key;
	char *cache;

	uint16_t argc = lua_gettop(L);
	uint16_t i;

	if (argc < 2) {
		return 0;
	}

	cache = (char *) lua_tostring(L, 1);

	if (!lua_checkstack(L, argc - 1)) {
		ulua_log("cache_exists_multi: too many items (%u) in the stack", argc - 1);
		return 0;
	}

	for (i = 2; i <= argc; i++) {

		key = (char *) lua_tolstring(L, i, &keylen);

		if (keylen && uwsgi_cache_magic_exists(key, keylen, cache)) {
			lua_pushboolean(L, 1);
		} else {
			lua_pushnil(L);
		}

	}

	return argc - 1;
}


static int uwsgi_api_signal_wait(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	uint16_t args = lua_gettop(L);
	int received_signal;

	wsgi_req->signal_received = -1;

	if (args > 0 && lua_isnumber(L, 1)) {
		received_signal = uwsgi_signal_wait(wsgi_req, lua_tonumber(L, 1));
	} else {
		received_signal = uwsgi_signal_wait(wsgi_req, -1);
	}

	if (received_signal < 0) {
		lua_pushnil(L);
	} else {
		wsgi_req->signal_received = received_signal;
		lua_pushnumber(L, received_signal);
	}

	return 1;
}

static int uwsgi_api_signal_received(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();

	lua_pushnumber(L, wsgi_req->signal_received);

	return 1;
}

static int uwsgi_api_signal_registered(lua_State *L) {

	uint16_t args = lua_gettop(L);
	struct uwsgi_signal_entry *use;
	uint8_t sig;

	if (args < 1 || !(lua_isnumber(L, 1))) {
		return 0;
	}

	sig = (uint8_t) lua_tonumber(L, 1);
	use = &uwsgi.shared->signal_table[sig];

	lua_pushboolean(L, use->handler ? 1 : 0);
	lua_pushstring(L, use->receiver);

	if (uwsgi.mywid > 0) {
		use = &uwsgi.shared->signal_table[sig + uwsgi.mywid*256];
	}

	lua_pushnumber(L, use->modifier1);

	return 3;
}

static int uwsgi_api_register_signal(lua_State *L) {

	// !! This is plugin specific function !!

	uint16_t args = lua_gettop(L);
	uint8_t sig;
	struct uwsgi_signal_entry *use;
	const char *who;
	size_t len;
	int i;

	if(!(uwsgi.master_process)) {
		ulua_log("no master, no signals");
		return 0;
	}

	if (args < 1 || !(lua_isnumber(L, 1))) {
		return 0;
	}

	sig = (uint8_t) lua_tonumber(L, 1);
	who = lua_tolstring(L, 2, &len);
	use = &uwsgi.shared->signal_table[sig];

	if (len == 0) {
		who = (const char *) &len; // len is zero anyway
	} else if (len > 63) {
		ulua_log("receiver lengh overflow: %s", who);
		return 0;
	}

	if (use->handler && use->modifier1 != 6) {
		ulua_log("signal %s has already been taken, but not by lua-plugin", sig);
	}

	// register signal's receiver on master
	if (!(use->handler) || strcmp(use->receiver, who)) {
		uwsgi_lock(uwsgi.signal_table_lock);

		// master-proc checks receiver
		strncpy(use->receiver, who, len+1);

		// if master has not any handler
		// if has, then only update receiver
		if (!(use->handler)) {
			use->handler = (void *) (1 /* unused */); // unsupported
			use->modifier1 = 6; // for future checks
		}

		// from non-lazy worker
		if (uwsgi.mywid == 0 && uwsgi.muleid == 0) {
			for(i = 1; i <= uwsgi.numproc; i++) {
				use = &uwsgi.shared->signal_table[sig + i*256];
				if (!(use->handler)) {
					use->handler = (void *) (1 /* unused */); // unsupported
					use->modifier1 = 6;
				}
			}
		}

		uwsgi_unlock(uwsgi.signal_table_lock);
	}

	// lazy (or runtime) worker, just register handler and modifier1
	if (uwsgi.mywid > 0) {
		use = &uwsgi.shared->signal_table[sig + uwsgi.mywid*256];

		if (use->modifier1 != 6) {
			uwsgi_lock(uwsgi.signal_table_lock);

			use->handler = (void *) (1 /* unused */); // unsupported
			use->modifier1 = 6;

			uwsgi_unlock(uwsgi.signal_table_lock);
		}
	}

	if (args > 2) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_SIGNAL_REF);
		lua_pushvalue(L, 1);
		lua_pushvalue(L, 3);
		lua_settable(L, -3);
	}

	ulua_log("signum %d registered (by %s %d, target: %s)", sig,
		!uwsgi.muleid ? "wid" : "mule", uwsgi.muleid || uwsgi.mywid, len ? who : "default");

	lua_pushboolean(L, 1);
	return 1;
}

static int uwsgi_api_add_file_monitor(lua_State *L) {
	uint16_t args = lua_gettop(L);
	uint8_t sig;
	const char *file;
	size_t len;

	if (args < 2 || !(lua_isnumber(L, 1))) {
		return 0;
	}

	sig = (uint8_t) lua_tonumber(L, 1);
	file = lua_tolstring(L, 2, &len);

	if (!len) {
		return 0;
	}

	if (!(uwsgi_add_file_monitor(sig, (char *) file))) {
		lua_pushboolean(L, 1);
		return 1;
	}

	return 0;

}

static int uwsgi_api_signal_add_timer(lua_State *L) {
	uint16_t args = lua_gettop(L);
	uint8_t sig;
	int secs;

	if (args < 2 || !(lua_isnumber(L, 1) && lua_isnumber(L, 2))) {
		return 0;
	}

	sig = (uint8_t) lua_tonumber(L, 1);
	secs = lua_tonumber(L, 2);

	if (!(uwsgi_add_timer(sig, secs))) {
		lua_pushboolean(L, 1);
		return 1;
	}

	return 0;
}

static int uwsgi_api_signal_add_rb_timer(lua_State *L) {
	uint16_t args = lua_gettop(L);
	uint8_t sig;
	int secs, itrs;

	if (args < 3 || !(lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3))) {
		return 0;
	}

	sig = (uint8_t) lua_tonumber(L, 1);
	secs = lua_tonumber(L, 2);
	itrs = lua_tonumber(L, 3);

	if (!(uwsgi_signal_add_rb_timer(sig, secs, itrs))) {
		lua_pushboolean(L, 1);
		return 1;
	}

	return 0;
}

static int uwsgi_api_signal_add_cron(lua_State *L) {
	int date[] = {-1, -1, -1, -1, -1};
	uint16_t args = lua_gettop(L);
	int i;

	if (args < 1 || !(lua_isnumber(L, 1))) {
		return 0;
	}

	if (args > 6) args = 6;

	for (i = 2; i <= args; i++) {
		if (lua_isnumber(L, i)) {
			date[i-2] = lua_tonumber(L, i);
		}
	}

	if (!(uwsgi_signal_add_cron((uint8_t) lua_tonumber(L, 1),
		date[0], date[1], date[2], date[3], date[4])))
	{
		lua_pushboolean(L, 1);
		return 1;
	}

	return 0;
}

static int uwsgi_api_alarm(lua_State *L) {
	uint16_t args = lua_gettop(L);
	const char *msg;
	size_t len;

	if (args < 2 || !(lua_isstring(L, 1))) {
		return 0;
	}

	if (uwsgi_lua_isutable(L, 2)) {
		uwsgi_lua_metatable_tostring(L, 1 - args);
	}

	msg = lua_tolstring(L, 2, &len);

	if (len) {
		uwsgi_alarm_trigger((char *) lua_tostring(L, 1), (char *) msg, len);
	}

	return 0;
}

static int uwsgi_api_async_sleep(lua_State *L) {
	uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

        int timeout = lua_tonumber(L, 1);

        if (timeout >= 0) {
                async_add_timeout(wsgi_req, timeout);
        }
end:
	lua_pushnil(L);
        return 1;
}

static int uwsgi_api_wait_fd_read(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

	int fd = lua_tonumber(L, 1);
	int timeout = 0;
	if (argc > 1) {
        	timeout = lua_tonumber(L, 2);
	}

	if (async_add_fd_read(wsgi_req, fd, timeout)) {
		lua_pushstring(L, "unable to call async_add_fd_read()");
        	lua_error(L);
        	return 0;
        }
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_wait_fd_write(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;

        struct wsgi_request *wsgi_req = current_wsgi_req();

        int fd = lua_tonumber(L, 1);
        int timeout = 0;
        if (argc > 1) {
                timeout = lua_tonumber(L, 2);
        }

        if (async_add_fd_write(wsgi_req, fd, timeout)) {
                lua_pushstring(L, "unable to call async_add_fd_write()");
                lua_error(L);
                return 0;
        }
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_async_connect(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;

	int fd = uwsgi_connect((char *)lua_tostring(L, 1), 0, 1);
	lua_pushnumber(L, fd);
	return 1;
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_is_connected(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;
	int fd = lua_tonumber(L, 1);
	if (uwsgi_is_connected(fd)) {
		lua_pushboolean(L, 1);
		return 1;
	}
	lua_pushboolean(L, 0);
        return 1;
end:
        lua_pushnil(L);
        return 1;
}

static int uwsgi_api_close(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto end;
        int fd = lua_tonumber(L, 1);
	close(fd);
end:
        lua_pushnil(L);
        return 1;
}


static int uwsgi_api_ready_fd(lua_State *L) {
	struct wsgi_request *wsgi_req = current_wsgi_req();
        int fd = uwsgi_ready_fd(wsgi_req);
        lua_pushnumber(L, fd);
        return 1;
}

static int uwsgi_api_websocket_handshake(lua_State *L) {
	uint16_t argc = lua_gettop(L);

	const char *key = NULL, *origin = NULL, *proto = NULL;
	size_t key_len = 0, origin_len = 0, proto_len = 0;

	if (argc > 0) {
		key = lua_tolstring(L, 1, &key_len);
		if (argc > 1) {
			origin = lua_tolstring(L, 2, &origin_len);
			if (argc > 2) {
				proto = lua_tolstring(L, 3, &proto_len);
			}
		}
	}

	struct wsgi_request *wsgi_req = current_wsgi_req();
	if (uwsgi_websocket_handshake(wsgi_req, (char *)key, key_len, (char *)origin, origin_len, (char *) proto, proto_len)) {
		goto error;
	}

	lua_pushnil(L);
        return 1;

error:
	lua_pushstring(L, "unable to complete websocket handshake");
	lua_error(L);
	return 0;
}

static int uwsgi_api_websocket_send(lua_State *L) {
	uint16_t argc = lua_gettop(L);
        if (argc == 0) goto error;

	size_t message_len = 0;

	if (uwsgi_lua_isutable(L, 1)) {
		uwsgi_lua_metatable_tostring(L, -argc);
	}

	const char *message = lua_tolstring(L, 1, &message_len);
	struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send(wsgi_req, (char *) message, message_len)) {
		goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message");
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_binary(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc == 0) goto error;

        size_t message_len = 0;

	if (uwsgi_lua_isutable(L, 1)) {
		uwsgi_lua_metatable_tostring(L, -argc);
	}

        const char *message = lua_tolstring(L, 1, &message_len);
        struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send_binary(wsgi_req, (char *) message, message_len)) {
                goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket binary message");
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_from_sharedarea(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc < 2) goto error;

	int id = lua_tonumber(L, 1);
	uint64_t pos = lua_tonumber(L, 2);
	uint64_t len = 0;
	if (argc > 2) {
		len = lua_tonumber(L, 3);
	}
        struct wsgi_request *wsgi_req = current_wsgi_req();

	if (uwsgi_websocket_send_from_sharedarea(wsgi_req, id, pos, len)) {
                goto error;
        }
	lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message from sharedarea");
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_send_binary_from_sharedarea(lua_State *L) {
        uint16_t argc = lua_gettop(L);
        if (argc < 2) goto error;

        int id = lua_tonumber(L, 1);
        uint64_t pos = lua_tonumber(L, 2);
        uint64_t len = 0;
        if (argc > 2) {
                len = lua_tonumber(L, 3);
        }
        struct wsgi_request *wsgi_req = current_wsgi_req();

        if (uwsgi_websocket_send_binary_from_sharedarea(wsgi_req, id, pos, len)) {
                goto error;
        }
        lua_pushnil(L);
        return 1;
error:
        lua_pushstring(L, "unable to send websocket message from sharedarea");
        lua_error(L);
        return 0;
}

static int uwsgi_api_websocket_recv(lua_State *L) {
	struct wsgi_request *wsgi_req = current_wsgi_req();
        struct uwsgi_buffer *ub = uwsgi_websocket_recv(wsgi_req);
	if (!ub) {
        	lua_pushstring(L, "unable to receive websocket message");
        	lua_error(L);
        	return 0;
	}
	lua_pushlstring(L, ub->buf, ub->pos);
	uwsgi_buffer_destroy(ub);
	return 1;
}

static int uwsgi_api_websocket_recv_nb(lua_State *L) {
        struct wsgi_request *wsgi_req = current_wsgi_req();
        struct uwsgi_buffer *ub = uwsgi_websocket_recv_nb(wsgi_req);
        if (!ub) {
                lua_pushstring(L, "unable to receive websocket message");
                lua_error(L);
                return 0;
        }
        lua_pushlstring(L, ub->buf, ub->pos);
        uwsgi_buffer_destroy(ub);
        return 1;
}

static int uwsgi_api_chunked_read(lua_State *L) {
	size_t len = 0;
	int timeout = lua_gettop(L) ? lua_tonumber(L, 1) : 0;
	struct wsgi_request *wsgi_req = current_wsgi_req();
	char *chunk = uwsgi_chunked_read(wsgi_req, &len, timeout, 0);
	if (!chunk) {
		lua_pushstring(L, "unable to receive chunked part");
		lua_error(L);
		return 0;
	}
	lua_pushlstring(L, chunk, len);
	return 1;
}

static int uwsgi_api_chunked_read_nb(lua_State *L) {
	size_t len = 0;
	struct wsgi_request *wsgi_req = current_wsgi_req();
	char *chunk = uwsgi_chunked_read(wsgi_req, &len, 0, 1);
	if (!chunk) {
		lua_pushstring(L, "unable to receive chunked part");
		lua_error(L);
		return 0;
	}
	lua_pushlstring(L, chunk, len);
	return 1;
}

static int uwsgi_lua_cache_magic_get(lua_State *L, uint8_t getnum) {

	uint16_t argc = lua_gettop(L);

	char *value;
	uint64_t valsize;

	char *key;
	size_t keylen;

	char *cache = NULL;

	size_t error;
	size_t tlen;
	size_t i;

	if (argc > 1) {
		cache = (char *) lua_tostring(L, 2);
	} else if (!argc) {
		return 0;
	}

	if (lua_istable(L, 1)) {

		error = 0;
		tlen = lua_rawlen(L, 1);

		lua_createtable(L, 0, tlen);

		for(i = 1; i <= tlen; i++) {

			lua_rawgeti(L, 1, i);

			key = (char *) lua_tolstring(L, -1, &keylen);

			value = (keylen) ? uwsgi_cache_magic_get(key, keylen, &valsize, NULL, cache) : NULL;

			if (value) {
				if (getnum) {
					lua_pushnumber(L, *((int64_t *) value));
				} else {
					lua_pushlstring(L, value, valsize);
				}
				free(value);
				lua_rawset(L, -3);
			} else {
				++error;
				lua_pop(L, 1);
			}

		}

		if (!error) {
			return 1;
		}

		lua_pushnumber(L, error);
		return 2;

	}

	key = (char *) lua_tolstring(L, 1, &keylen);

	value = (keylen) ? uwsgi_cache_magic_get(key, keylen, &valsize, NULL, cache) : NULL;

	if (value) {
		if (getnum) {
			lua_pushnumber(L, *((int64_t *) value));
		} else {
			lua_pushlstring(L, value, valsize);
		}
		free(value);
	} else {
		lua_pushnil(L);
	}

	return 1;

}

static int uwsgi_api_cache_get(lua_State *L) {
	return uwsgi_lua_cache_magic_get(L, 0);
}

static int uwsgi_api_cache_getnum(lua_State *L) {
	return uwsgi_lua_cache_magic_get(L, 1);
}

static int uwsgi_lua_cache_magic_get_multi(lua_State *L, uint8_t getnum) {

	char *value ;
	uint64_t valsize;

	char *key;
	size_t keylen;

	char *cache;

	uint16_t argc = lua_gettop(L);
	uint16_t i;

	if (argc < 2) {
		return 0;
	}

	if (!lua_checkstack(L, argc - 1)) {
		ulua_log("cache_get_multi: too many items (%u) in the stack", argc - 1);
		return 0;
	}

	cache = (char *) lua_tostring(L, 1);

	for (i = 2; i <= argc; i++) {

		key = (char *) lua_tolstring(L, i, &keylen);

		value = (keylen) ? uwsgi_cache_magic_get(key, keylen, &valsize, NULL, cache) : NULL;

		if (value) {
			if (getnum) {
				lua_pushnumber(L, *((int64_t *) value));
			} else {
				lua_pushlstring(L, value, valsize);
			}
			free(value);
		} else {
			lua_pushnil(L);
		}
	}

	return argc - 1;
}

static int uwsgi_api_cache_get_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_get_multi(L, 0);
}

static int uwsgi_api_cache_getnum_multi(lua_State *L) {
	return uwsgi_lua_cache_magic_get_multi(L, 1);
}

static int uwsgi_lua_cache_magic_get_tmulti(lua_State *L, uint8_t getnum) {

	char *value ;
	uint64_t valsize;

	char *key;
	size_t keylen;

	char *cache;

	uint16_t argc = lua_gettop(L);
	uint16_t error = 0;
	uint16_t i;

	if (argc < 2) {
		return 0;
	}

	cache = (char *) lua_tostring(L, 1);

	lua_createtable(L, 0, argc - 1);

	for (i = 2; i <= argc; i++) {

		key = (char *) lua_tolstring(L, i, &keylen);

		value = (keylen) ? uwsgi_cache_magic_get(key, keylen, &valsize, NULL, cache) : NULL;

		if (value) {
			if (getnum) {
				lua_pushnumber(L, *((int64_t *) value));
			} else {
				lua_pushlstring(L, value, valsize);
			}
			free(value);
			lua_setfield(L, -2, key);
		} else {
			++error;
		}

	}

	if (!error) {
		return 1;
	}

	lua_pushnumber(L, error);
	return 2;

}

static int uwsgi_api_cache_get_tmulti(lua_State *L) {
	return uwsgi_lua_cache_magic_get_tmulti(L, 0);
}

static int uwsgi_api_cache_getnum_tmulti(lua_State *L) {
	return uwsgi_lua_cache_magic_get_tmulti(L, 1);
}


static int uwsgi_api_req_fd(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();

	lua_pushnumber(L, wsgi_req->fd);
	return 1;
}

static int uwsgi_lua_lock(lua_State *L, uint8_t op) {

	int lock_num = 0;

	// the spooler cannot lock resources
	if (uwsgi.i_am_a_spooler) {
		lua_pushstring(L, "The spooler cannot lock/unlock resources");
		lua_error(L);
	}

	if (lua_gettop(L) > 0) {
		lock_num = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
		if (lock_num < 0 || lock_num > uwsgi.locks) {
			lua_pushstring(L, "Invalid lock number");
			lua_error(L);
		}
	}

	switch (op) {
		case ULUA_LOCK_LOCK:
			uwsgi_lock(uwsgi.user_lock[lock_num]); break;
		case ULUA_LOCK_UNLOCK:
			uwsgi_unlock(uwsgi.user_lock[lock_num]); break;
		case ULUA_LOCK_CHECK:
			lua_pushboolean(L, uwsgi_lock_check(uwsgi.user_lock[lock_num])); return 1;
	}

	return 0;
}

static int uwsgi_api_is_locked(lua_State *L) {
	return uwsgi_lua_lock(L, ULUA_LOCK_CHECK);
}

static int uwsgi_api_lock(lua_State *L) {
	return uwsgi_lua_lock(L, ULUA_LOCK_LOCK);
}

static int uwsgi_api_unlock(lua_State *L) {
	return uwsgi_lua_lock(L, ULUA_LOCK_UNLOCK);
}

static int uwsgi_lua_input(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();
	ssize_t sum = 0;

	int n = lua_gettop(L);

	if (n > 1) {
		sum = lua_tonumber(L, 2);
	}

	ssize_t rlen = 0;

        char *buf = uwsgi_request_body_read(wsgi_req, sum, &rlen);
        if (buf) {
		lua_pushlstring(L, buf, rlen);
                return 1;
        }

	return 0;
}


static int uwsgi_api_async_id_get(lua_State *L) {

	struct wsgi_request *wsgi_req = current_wsgi_req();

	lua_pushnumber(L, wsgi_req->async_id);

	return 1;
}

static int uwsgi_api_memory_usage(lua_State *L) {

	uint64_t rss = 0;
	uint64_t vsz = 0;

	get_memusage(&rss, &vsz);

	lua_pushnumber(L, rss);
	lua_pushnumber(L, vsz);

	return 2;
}

static int uwsgi_api_setprocname(lua_State *L) {

	if (lua_gettop(L) > 0 && lua_isstring(L, 1)) {
		uwsgi_set_processname((char *) lua_tostring(L, 1));
	}

	return 0;
}

static int uwsgi_api_pid(lua_State *L) {

	int id = 0;

	if (lua_gettop(L) > 0) {
		id = lua_tonumber(L, 1);
	}

	if (id >= 0 && id <= uwsgi.numproc) {
		lua_pushnumber(L, uwsgi.workers[id].pid);
		return 1;
	}

	return 0;

}

static int uwsgi_api_mypid(lua_State *L) {

	lua_pushnumber(L, uwsgi.mypid);

	return 1;
}

static int uwsgi_api_mymid(lua_State *L) {

	lua_pushnumber(L, uwsgi.muleid);

	return 1;
}

static int uwsgi_api_mywid(lua_State *L) {

	lua_pushnumber(L, uwsgi.mywid);

	return 1;
}

static int uwsgi_api_mule_msg_get(lua_State *L) {

	uint16_t argc = lua_gettop(L);

	int manage_signals = 1;
	int manage_farms = 1;
	int timeout = -1;

	char *msg;

	size_t buffer_size = ULUA_MULE_MSG_GET_BUFFER_SIZE;
	ssize_t len = 0;

	if (argc > 0 && lua_istable(L, 1)) {
		lua_pop(L, argc - 1);

		lua_getfield(L, 1, "signals");
		lua_getfield(L, 1, "farms");
		lua_getfield(L, 1, "timeout");
		lua_getfield(L, 1, "buffer_size");

		lua_remove(L, -5);
		argc = 4;
	}

	switch(argc > 4 ? 4 : argc) {
		case 4: if (lua_isnumber(L, 4)) buffer_size = lua_tonumber(L, 4);
		case 3: if (lua_isnumber(L, 3)) timeout = lua_tonumber(L, 3);
		case 2: if (!lua_isnil(L, 2)) manage_farms = lua_toboolean(L, 2);
		case 1: if (!lua_isnil(L, 1)) manage_signals = lua_toboolean(L, 1);
	}

	msg = (char *) uwsgi_malloc(buffer_size);

	len = uwsgi_mule_get_msg(manage_signals, manage_farms, msg, buffer_size, timeout);

	if (len < 0) {
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, msg, len);
	}

	free(msg);

	return 1;
}

static int uwsgi_api_mule_msg(lua_State *L) {

	int fd;
	int mule_id;
	int type;

	size_t msglen;
	char *msg;

	uint16_t argc = lua_gettop(L);

	if (argc < 1 || !lua_isstring(L, 1)) {
		return 0;
	}

	if (uwsgi.mules_cnt < 1) {
		ulua_log("mule_msg: no mules. no send");
		return 0;
	}

	msg = (char *) lua_tolstring(L, 1, &msglen);

	if (argc == 1) {
		mule_send_msg(uwsgi.shared->mule_queue_pipe[0], msg, msglen);
	} else {
		type = lua_type(L, 2);

		if (type == LUA_TSTRING) {

			struct uwsgi_farm *uf = get_farm_by_name((char *)lua_tostring(L, 2));

			if (!uf) {
				ulua_log("mule_msg: unknown farm");
				return 0;
			}

			fd = uf->queue_pipe[0];

		} else if (type == LUA_TNUMBER) {

			mule_id = lua_tonumber(L, 2);

			if (mule_id < 0 && mule_id > uwsgi.mules_cnt) {
				ulua_log("mule_msg: mule_id is out of range");
				return 0;
			}

			if (mule_id == 0) {
				fd = uwsgi.shared->mule_queue_pipe[0];
			} else {
				fd = uwsgi.mules[mule_id-1].queue_pipe[0];
			}

		} else {
			fd = -1;
			ulua_log("mule_msg: invalid mule");
		}

		if (fd > -1) {
			mule_send_msg(fd, msg, msglen);
		}
	}

	return 0;
}

static int uwsgi_api_queue_push(lua_State *L) {

	size_t len;
	char *str;

	uint16_t argc = lua_gettop(L);
	uint16_t error = 0;

	char **list;
	size_t *slen;
	uint16_t i;


	if (!argc || !uwsgi.queue_size) {
		return 0;
	}

	if (argc == 1) {

		if (uwsgi_lua_isutable(L, 1)) {
			uwsgi_lua_metatable_tostring(L, -1);
		}

		str = (char *) lua_tolstring(L, 1, &len);

		if (len) {
			uwsgi_wlock(uwsgi.queue_lock);

			if (uwsgi_queue_push(str, len)) {
				++error;
			}

			uwsgi_rwunlock(uwsgi.queue_lock);
		} else {
			++error;
		}

	} else {

		list = (char **) uwsgi_malloc(sizeof(char *) * argc);
		slen = (size_t *) uwsgi_malloc(sizeof(size_t) * argc);

		for (i = 0; i < argc; ++i) {

			if (uwsgi_lua_isutable(L, i + 1)) {
				uwsgi_lua_metatable_tostring(L, i - argc);
			}

			list[i] = (char *) lua_tolstring(L, i + 1, &slen[i]);

		}

		uwsgi_wlock(uwsgi.queue_lock);

		for (i = 0; i < argc; ++i) {

			if (!slen[i] || uwsgi_queue_push(list[i], slen[i])) {
				++error;
			}

		}

		uwsgi_rwunlock(uwsgi.queue_lock);

		free(list);
		free(slen);

	}

	if (!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;

}

static int uwsgi_api_queue_set(lua_State *L) {

	uint64_t key;
	uint64_t len;
	char *str;
	uint16_t i;

	uint16_t error = 0;
	uint16_t argc = lua_gettop(L);

	if (!(argc && uwsgi.queue_size)) {
		return 0;
	}

	if (lua_istable(L, 1)) {

		lua_pushnil(L);

		while(lua_next(L, 1)) {
			if (lua_isnumber(L, -2)) {

				if (uwsgi_lua_isutable(L, -1)) {
					uwsgi_lua_metatable_tostring(L, -1);
				}

				str = (char *) lua_tolstring(L, -1, &len);
				key = lua_tonumber(L, -2);

				if (len) {
					uwsgi_wlock(uwsgi.queue_lock);

					if (!uwsgi_queue_set(key, str, len)) {
						error++;
					}

					uwsgi_rwunlock(uwsgi.queue_lock);
				} else {
					error++;
				}

			} else {
				error++;
			}

			lua_pop(L, 1);
		}

	} else {

		for (i = 2; i <= argc; i+=2) {
			if (lua_isnumber(L, i - 1)) {

				if (uwsgi_lua_isutable(L, i)) {
					uwsgi_lua_metatable_tostring(L, i - argc - 1);
				}

				key = lua_tonumber(L, i - 1);
				str = (char *) lua_tolstring(L, i, &len);

				if (len) {
					uwsgi_wlock(uwsgi.queue_lock);

					if (!uwsgi_queue_set(key, str, len)) {
						error++;
					}

					uwsgi_rwunlock(uwsgi.queue_lock);
				} else {
					error++;
				}

			} else {
				error++;
			}
		}

	}

	if (!error) {
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushnil(L);
	lua_pushnumber(L, error);

	return 2;
}

static int uwsgi_api_queue_slot(lua_State *L) {

	lua_pushnumber(L, uwsgi.queue_header->pos);

	return 1;
}

static int uwsgi_api_queue_pull_slot(lua_State *L) {

	lua_pushnumber(L, uwsgi.queue_header->pull_pos);

	return 1;
}

static int uwsgi_lua_queue_pull_pop(lua_State *L, char* queue(uint64_t*)) {

	char *str;
	size_t len;
	int i;

	int num = (lua_gettop(L) > 0) ? lua_tonumber(L, 1) : 1;

	if (!uwsgi.queue_size) {
		return 0;
	}

	if (!lua_checkstack(L, num)) {
		ulua_log("queue_pop(pull): too many items (%u) in the stack", num);
		return 0;
	}

	uwsgi_wlock(uwsgi.queue_lock);

	for(i = num; i > 0; --i) {

		len = 0;
		str = queue(&len);

		if (len) {
			lua_pushlstring(L, str, len);
		} else {
			lua_pushnil(L);
		}
	}

	uwsgi_rwunlock(uwsgi.queue_lock);

	return num;
}

static int uwsgi_api_queue_pull(lua_State *L) {
	return uwsgi_lua_queue_pull_pop(L, uwsgi_queue_pull);
}

static int uwsgi_api_queue_pop(lua_State *L) {
	return uwsgi_lua_queue_pull_pop(L, uwsgi_queue_pop);
}

static int uwsgi_api_queue_get(lua_State *L) {

	char *str;
	size_t len;

	uint16_t i;
	uint16_t argc = lua_gettop(L);

	uint64_t key;
	uint64_t *list;

	if (!argc || !uwsgi.queue_size) {
		return 0;
	}

	if (argc == 1) {
		key = lua_tonumber(L, 1);

		uwsgi_rlock(uwsgi.queue_lock);

		str = uwsgi_queue_get(key, &len);

		if (len) {
			lua_pushlstring(L, str, len);
		} else {
			lua_pushnil(L);
		}

		uwsgi_rwunlock(uwsgi.queue_lock);
	} else {

		list = (uint64_t *) uwsgi_malloc(sizeof(uint64_t) * argc);

		for (i = 0; i < argc; ++i) {
			list[i] = lua_tonumber(L, i + 1);
		}

		lua_pop(L, argc);

		uwsgi_rlock(uwsgi.queue_lock);

		for (i = 0; i < argc; ++i) {
			str = uwsgi_queue_get(list[i], &len);

			if (len) {
				lua_pushlstring(L, str, len);
			} else {
				lua_pushnil(L);
			}
		}

		uwsgi_rwunlock(uwsgi.queue_lock);

		free(list);
	}

	return argc;
}

static int uwsgi_api_queue_last(lua_State *L) {

	char *str;
	size_t len;
	uint64_t base;

	unsigned int left;
	unsigned int count = lua_gettop(L) ? lua_tonumber(L, 1) : 0;

	if (uwsgi.queue_size) {

		uwsgi_rlock(uwsgi.queue_lock);

		base = uwsgi.queue_header->pos > 0 ? uwsgi.queue_header->pos-1 : uwsgi.queue_size-1;

		if (!(count)) {

			str = uwsgi_queue_get(base, &len);

			if (len) {
				lua_pushlstring(L, str, len);
				uwsgi_rwunlock(uwsgi.queue_lock);
				return 1;
			}

			uwsgi_rwunlock(uwsgi.queue_lock);
			return 0;
		}

		if (count > uwsgi.queue_size) {
			count = uwsgi.queue_size;
		}

		if (!lua_checkstack(L, count)) {
			ulua_log("queue_last: too many items (%u) in the stack", count);
			uwsgi_rwunlock(uwsgi.queue_lock);
			return 0;
		}

		left = count;

		while(left) {
			str = uwsgi_queue_get(base, &len);

			if(len) {
				lua_pushlstring(L, str, len);
			} else {
				lua_pushnil(L);
			}

			if (base > 0) {
				--base;
			} else {
				base = uwsgi.queue_size-1;
			}

			--left;
		}

		uwsgi_rwunlock(uwsgi.queue_lock);
		return count;
	}

	return 0;
}

static int uwsgi_api_mule_msg_hook(lua_State *L) {

	if (!lua_gettop(L)) {
		lua_pushnil(L);
	} else {
		lua_pushvalue(L, 1);
	}

	lua_rawseti(L, LUA_REGISTRYINDEX, ULUA_MULE_MSG_REF);

	return 0;
}

// basic api list
static const luaL_Reg uwsgi_api_base[] = {
  {"log", uwsgi_api_log},

  {"metric_get", uwsgi_api_metric_get},
  {"metric_set", uwsgi_api_metric_set},
  {"metric_inc", uwsgi_api_metric_inc},
  {"metric_div", uwsgi_api_metric_div},
  {"metric_mul", uwsgi_api_metric_mul},
  {"metric_dec", uwsgi_api_metric_dec},

  {"cache_get", uwsgi_api_cache_get},
  {"cache_get_multi", uwsgi_api_cache_get_multi},
  {"cache_get_tmulti", uwsgi_api_cache_get_tmulti},
  {"cache_getnum", uwsgi_api_cache_getnum},
  {"cache_getnum_multi", uwsgi_api_cache_getnum_multi},
  {"cache_getnum_tmulti", uwsgi_api_cache_getnum_tmulti},
  {"cache_set", uwsgi_api_cache_set},
  {"cache_set_multi", uwsgi_api_cache_set_multi},
  {"cache_setnum", uwsgi_api_cache_setnum},
  {"cache_setnum_multi", uwsgi_api_cache_setnum_multi},
  {"cache_update", uwsgi_api_cache_update},
  {"cache_update_multi", uwsgi_api_cache_update_multi},
  {"cache_updatenum", uwsgi_api_cache_updatenum},
  {"cache_updatenum_multi", uwsgi_api_cache_updatenum_multi},
  {"cache_inc", uwsgi_api_cache_inc},
  {"cache_inc_multi", uwsgi_api_cache_inc_multi},
  {"cache_dec", uwsgi_api_cache_dec},
  {"cache_dec_multi", uwsgi_api_cache_dec_multi},
  {"cache_mul", uwsgi_api_cache_mul},
  {"cache_mul_multi", uwsgi_api_cache_mul_multi},
  {"cache_div", uwsgi_api_cache_div},
  {"cache_div_multi", uwsgi_api_cache_div_multi},
  {"cache_del", uwsgi_api_cache_del},
  {"cache_del_multi", uwsgi_api_cache_del_multi},
  {"cache_exists", uwsgi_api_cache_exists},
  {"cache_exists_multi", uwsgi_api_cache_exists_multi},
  {"cache_clear", uwsgi_api_cache_clear},
  {"cache_keys", uwsgi_api_cache_keys},

  {"queue_push", uwsgi_api_queue_push},
  {"queue_set", uwsgi_api_queue_set},
  {"queue_slot", uwsgi_api_queue_slot},
  {"queue_pull_slot", uwsgi_api_queue_pull_slot},
  {"queue_pop",uwsgi_api_queue_pop},
  {"queue_get",uwsgi_api_queue_get},
  {"queue_pull",uwsgi_api_queue_pull},
  {"queue_last",uwsgi_api_queue_last},

  {"register_signal", uwsgi_api_register_signal},
  {"signal_registered", uwsgi_api_signal_registered},
  {"signal_wait", uwsgi_api_signal_wait},
  {"signal_received", uwsgi_api_signal_received},
  {"add_file_monitor", uwsgi_api_add_file_monitor},
  {"add_timer", uwsgi_api_signal_add_timer},
  {"add_rb_timer", uwsgi_api_signal_add_rb_timer},
  {"add_cron", uwsgi_api_signal_add_cron},

  {"alarm", uwsgi_api_alarm},
  {"signal", uwsgi_api_signal},

  {"mem", uwsgi_api_memory_usage},
  {"pid", uwsgi_api_pid},
  {"mypid", uwsgi_api_mypid},
  {"micros", uwsgi_api_micros},

  {"setprocname", uwsgi_api_setprocname},

  {"lock", uwsgi_api_lock},
  {"is_locked", uwsgi_api_is_locked},
  {"unlock", uwsgi_api_unlock},
  {"reload", uwsgi_api_reload},
  {"stop", uwsgi_api_stop},

  {"mule_msg", uwsgi_api_mule_msg},

  {NULL, NULL}
};

// worker only adds
static const luaL_Reg uwsgi_api_worker[] = {

  {"mywid", uwsgi_api_mywid},
  {"connection_fd", uwsgi_api_req_fd},

  {"register_rpc", uwsgi_api_register_rpc},
  {"rpc", uwsgi_api_rpc},

  {"req_input_read", uwsgi_lua_input},
  {"chunked_read", uwsgi_api_chunked_read},
  {"chunked_read_nb", uwsgi_api_chunked_read_nb},
  {"request_id", uwsgi_api_request_id},

  {"async_sleep", uwsgi_api_async_sleep},
  {"async_connect", uwsgi_api_async_connect},
  {"async_id", uwsgi_api_async_id_get},
  {"is_connected", uwsgi_api_is_connected},
  {"wait_fd_read", uwsgi_api_wait_fd_read},
  {"wait_fd_write", uwsgi_api_wait_fd_write},

  {"websocket_handshake", uwsgi_api_websocket_handshake},
  {"websocket_recv", uwsgi_api_websocket_recv},
  {"websocket_recv_nb", uwsgi_api_websocket_recv_nb},
  {"websocket_send", uwsgi_api_websocket_send},
  {"websocket_send_from_sharedarea", uwsgi_api_websocket_send_from_sharedarea},
  {"websocket_send_binary", uwsgi_api_websocket_send_binary},
  {"websocket_send_binary_from_sharedarea", uwsgi_api_websocket_send_binary_from_sharedarea},

  {"close", uwsgi_api_close},
  {"ready_fd", uwsgi_api_ready_fd},

  {NULL, NULL}
};

// mule only adds
static const luaL_Reg uwsgi_api_mule[] = {

  {"mymid", uwsgi_api_mymid},
  {"mule_msg_get", uwsgi_api_mule_msg_get},
  {"mule_msg_hook", uwsgi_api_mule_msg_hook},

  {NULL, NULL}
};

static void uwsgi_lua_api_newglobaltable(lua_State *L, const char *name) {
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_setglobal(L, name);
}

static void uwsgi_lua_api_push(lua_State *L, int target, const luaL_Reg *api) {
	for (; api->name; api++) {
		lua_pushstring(L, api->name);
		lua_pushcfunction(L, api->func);
		lua_rawset(L, target - 2);
	}
}

static int uwsgi_lua_init() {

	int i;

	if(!(ulua.gc_perform)) {
		ulua.gc_perform = LUA_GCSTEP;
	} else {
		ulua.gc_perform = LUA_GCCOLLECT;
	}

	if (ULUA_WORKER_ANYAPP) {
		uwsgi_log(ULUA_LOG_HEADER " Initializing Worker's Lua environment... ");

		ulua.state = uwsgi_malloc(sizeof(lua_State**) * uwsgi.numproc);

		for (i = 0; i<uwsgi.numproc; i++) {
			ulua.state[i] = uwsgi_malloc(sizeof(lua_State*) * uwsgi.cores);
		}

		uwsgi_log("%d lua_State(s) (with %d lua_Thread(s))\n", uwsgi.numproc, uwsgi.cores);
	}

	if (uwsgi.mules_cnt > 0) {
		uwsgi_log(ULUA_LOG_HEADER " Initializing Mule's Lua environment... ");

		ulua.mulestate = uwsgi_malloc(sizeof(lua_State*) * uwsgi.mules_cnt);

		for (i = 0; i<uwsgi.mules_cnt; i++) {
			ulua.mulestate[i] = NULL;
		}

		uwsgi_log("%d lua_State(s) (for %d mule(s))\n", uwsgi.mules_cnt, uwsgi.mules_cnt);
	}

	// ok the lua engine is ready
	return 0;
}

static void uwsgi_lua_init_state(lua_State **Ls, int wid, int sid, int cores) {

	if (!ULUA_WORKER_ANYAPP) {
		return;
	}

	int i;
	int uslnargs;
	lua_State *L;

	// spawn worker state
	Ls[0] = luaL_newstate();
	L = Ls[0];

	// init worker state
	luaL_openlibs(L);

	uwsgi_lua_api_newglobaltable(L, "uwsgi");
	uwsgi_lua_api_push(L, -1, uwsgi_api_base);
	uwsgi_lua_api_push(L, -1, uwsgi_api_worker);

	lua_pushstring(L, UWSGI_VERSION);
	lua_setfield(L, -2, "version");

	lua_pushnumber(L, sid);
	lua_setfield(L, -2, "mysid");

	// reserve ref 1 for ws func
	lua_pushboolean(L, 0);
	luaL_ref(L, LUA_REGISTRYINDEX);

	// signal table ref 2
	lua_newtable(L);
	lua_pushvalue(L, -1);

	luaL_ref(L, LUA_REGISTRYINDEX);
	lua_setfield(L, -2, "signal_ref");

	// rpc metatable ref 3
	lua_newtable(L);
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, uwsgi_api_register_rpc_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);

	luaL_ref(L, LUA_REGISTRYINDEX);
	lua_setfield(L, -2, "rpc_ref");

	lua_pop(L, 1);

	//init additional threads for current worker
	if (cores > 0) {

		lua_createtable(L, cores - 1, 0);

		for(i = 1; i < cores; i++) {

			// create thread and save it
			Ls[i] = lua_newthread(L);
			lua_rawseti(L, -2, i);

		}

		luaL_ref(L, LUA_REGISTRYINDEX); // ref for threads (not reserved!)
	}

	// init main app
	uslnargs = lua_gettop(L);

	struct uwsgi_string_list *usl = ulua.load;

	while(usl) {
		if (luaL_dofile(L, usl->value)) {
			ulua_log("unable to load Lua file %s: %s", usl->value, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		usl = usl->next;
	}

	uslnargs = lua_gettop(L) - uslnargs;

	if (ulua.wsapi) {
		if (luaL_loadfile(L, ulua.wsapi)) {
			ulua_log("unable to load Lua file %s: %s", ulua.wsapi, lua_tostring(L, -1));
			lua_pop(L, uslnargs + 1);
			uslnargs = 0;
		} else {
			// put function before args
			if (uslnargs > 0) {
				lua_insert(L, -uslnargs - 1);
			}

			if (lua_pcall(L, uslnargs, 1, 0)) {
				ulua_log("unable to load Lua file %s: %s", ulua.wsapi, lua_tostring(L, -1));
				lua_pop(L, 1);
				uslnargs = 0;
			} else {
				uslnargs = 1;
			}
		}
	} else {
		lua_pop(L, uslnargs);
		uslnargs = 0;
	}

	// table ??
	if (uslnargs > 0 && lua_istable(L, -1)) {
		lua_pushstring(L, "run");
		lua_gettable(L, -1);
		lua_replace(L, -1);
	}

	// no app ???
	if (!uslnargs || !lua_isfunction(L, -1)) {
		lua_pop(L, uslnargs);
		ulua_log("Can't find WSAPI entry point (no function, nor a table with function'run').");
		ulua.wsapi = 0;
	} else {
		lua_rawseti(L, LUA_REGISTRYINDEX, ULUA_WSAPI_REF);
	}

	// post load
	usl = ulua.postload;

	while(usl) {
		if (luaL_loadfile(L, usl->value) || lua_pcall(L, 0, 0, 0)) {
			ulua_log("unable to load Lua file %s: %s", usl->value, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		usl = usl->next;
	}

	// and the worker is ready!
	lua_gc(L, LUA_GCCOLLECT, 0);
}

static void uwsgi_lua_request_headers_nested_tables(lua_State *L, int8_t table, struct wsgi_request *wsgi_req, char *header, size_t len) {

	size_t i, rlen;
	char *rheader;

	size_t tlen = lua_rawlen(L, table);

	for (i = 1; i <= tlen; i++) {
		lua_rawgeti(L, table, i);

		if (lua_istable(L, -1)) {
			uwsgi_lua_request_headers_nested_tables(L, -1, wsgi_req, header, len);
		} else {
			rheader = (char *) lua_tolstring(L, -1, &rlen);
			uwsgi_response_add_header(wsgi_req, header, len, rheader, rlen);
		}

		lua_pop(L, 1);
	}
}

static int uwsgi_lua_request(struct wsgi_request *wsgi_req) {

	const char *http, *http2;
	size_t i, slen, slen2;
	int8_t t_err;

	if(!ulua.wsapi) {
		ulua_log("worker%d[%d]: No WSAPI App. skip.", uwsgi.mywid, wsgi_req->async_id);
		return -1;
	}

	lua_State *L = ULUA_WORKER_STATE[wsgi_req->async_id];

	/* Standard WSAPI request */
	if (!wsgi_req->len) {
		ulua_log("worker%d[%d]: Empty lua request. skip.", uwsgi.mywid, wsgi_req->async_id);
		return -1;
	}

	if (wsgi_req->async_status == UWSGI_AGAIN) {
		while (!lua_pcall(L, 0, 1, 0)) {
			switch(lua_type(L, -1)) {
				case LUA_TNIL: // posible dead coroutine

					lua_pop(L, 1);
					lua_pushvalue(L, -1);
					continue; // retry

				case LUA_TUSERDATA:
				case LUA_TTABLE:
					t_err = uwsgi_lua_metatable_tostring_protected(L, -1);
					if (t_err <= 0) { if (!t_err) break; goto clear; } // tostring exception?

				case LUA_TSTRING:
				case LUA_TNUMBER:

					http = lua_tolstring(L, -1, &slen);
					uwsgi_response_write_body_do(wsgi_req, (char *)http, slen);

			}

			lua_pop(L, 1);
			lua_pushvalue(L, -1);
			return UWSGI_AGAIN;
		}
		goto clear;
	}

	if (uwsgi_parse_vars(wsgi_req)) {
		return -1;
	}

	// put function in the stack
	lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_WSAPI_REF);

	// put cgi vars in the stack
	lua_createtable(L, 0, wsgi_req->var_cnt + 2);

	lua_pushstring(L, "");
	lua_setfield(L, -2, "CONTENT_TYPE");

	for(i = 0; i < wsgi_req->var_cnt; i+=2) {
		lua_pushlstring(L, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len);
		lua_pushlstring(L, wsgi_req->hvec[i+1].iov_base, wsgi_req->hvec[i+1].iov_len);
		lua_rawset(L, -3);
	}

	// put "input" table
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, uwsgi_lua_input);
	lua_setfield(L, -2, "read");
	lua_setfield(L, -2, "input");


#ifdef UWSGI_DEBUG
	ulua_log("stack pos %d", lua_gettop(L));
#endif

	// call function
	if (lua_pcall(L, 1, 4, 0)) {
		ulua_log("worker%d[%d]: %s", uwsgi.mywid, wsgi_req->async_id, lua_tostring(L, -1));
		lua_pop(L, 1);
		goto clear2;
	}

#ifdef UWSGI_DEBUG
	ulua_log("%s %s %s %s", lua_typename(L, lua_type(L, -4)), lua_typename(L, lua_type(L, -3)), lua_typename(L, lua_type(L, -2)) ,  lua_typename(L, lua_type(L, -1)));
#endif

	// send status
	http = lua_tolstring(L, -4, &slen);

	if (!slen) {
		ulua_log("worker%d[%d]: invalid response status!", uwsgi.mywid, wsgi_req->async_id);
	}

	if (uwsgi_response_prepare_headers(wsgi_req, (char *) http, slen)) {
		lua_pop(L, 4);
		return -1;
	}

	// send headers
	if (lua_istable(L, -3)) {

		lua_pushnil(L);

		while(lua_next(L, -4)) {

			// lua_tolstring may change the 'lua_next' sequence, if the key is not a string, by modifying it
			lua_pushvalue(L, -2);
			// so -1 is key, -2 is value
			http = lua_tolstring(L, -1, &slen);

			if (lua_istable(L, -2)) {

				uwsgi_lua_request_headers_nested_tables(L, -2, wsgi_req, (char *) http, slen);
			} else {

				http2 = lua_tolstring(L, -2, &slen2);
				uwsgi_response_add_header(wsgi_req, (char *) http, slen, (char *) http2, slen2);
			}

			lua_pop(L, 2);
		}
	}

	// send body with coroutine or copy from string

	switch(lua_type(L, -2)) {
		case LUA_TFUNCTION:

			// initing coroutine:
			// pushing first argument of coroutine and call it fisrt time:

			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);

			if(!lua_pcall(L, 1, 1, 0)) {
				switch(lua_type(L, -1)) {
					case LUA_TUSERDATA:
					case LUA_TTABLE:
						t_err = uwsgi_lua_metatable_tostring_protected(L, -1);
						if (t_err <= 0) { if (!t_err) break; lua_pop(L, 1); goto clear; } // tostring exception?

					case LUA_TSTRING:
					case LUA_TNUMBER:

						http = lua_tolstring(L, -1, &slen);
						uwsgi_response_write_body_do(wsgi_req, (char *) http, slen);

				}
			} else {
				ulua_log("worker%d[%d]: init-coroutine: %s", uwsgi.mywid, wsgi_req->async_id, lua_tostring(L, -1));
				lua_pop(L, 1);
				break;
			}

			lua_pop(L, 2);
			lua_pushvalue(L, -1);

			// continue coroutine

			if (uwsgi.async > 0) {
				return UWSGI_AGAIN;
			}

			while (!lua_pcall(L, 0, 1, 0)) {
				switch(lua_type(L, -1)) {
					case LUA_TUSERDATA:
					case LUA_TTABLE:
						t_err = uwsgi_lua_metatable_tostring_protected(L, -1);
						if (t_err <= 0) { if (!t_err) break; goto clear; } // tostring exception?

					case LUA_TSTRING:
					case LUA_TNUMBER:

						http = lua_tolstring(L, -1, &slen);
						uwsgi_response_write_body_do(wsgi_req, (char *) http, slen);

				}

				lua_pop(L, 1);
				lua_pushvalue(L, -1);
			}

			break;

		case LUA_TUSERDATA:
		case LUA_TTABLE: if(uwsgi_lua_metatable_tostring_protected(L, -2) <= 0) break;
		case LUA_TSTRING:
		case LUA_TNUMBER:

			http = lua_tolstring(L, -2, &slen);
			uwsgi_response_write_body_do(wsgi_req, (char *) http, slen);
	}

clear:
	lua_pop(L, 4);
clear2:
	// set frequency
	if (ulua.gc_freq && (ulua.gc_freq == 1 ||
		(uwsgi.threads == 1 && uwsgi.workers[uwsgi.mywid].requests % ulua.gc_freq == 0) ||
		(uwsgi.threads > 1 && uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].requests % ulua.gc_freq == 0)))
	{
		lua_gc(L, ulua.gc_perform, 0);
	}

	return UWSGI_OK;

}

static void uwsgi_lua_after_request(struct wsgi_request *wsgi_req) {

	log_request(wsgi_req);
}


static int uwsgi_lua_magic(char *mountpoint, char *lazy) {

	if( !strcmp(lazy+strlen(lazy)-4, ".lua")  ||
		!strcmp(lazy+strlen(lazy)-5, ".luac") ||
		!strcmp(lazy+strlen(lazy)-3, ".ws"))
	{
		ulua.wsapi = lazy;
		return 1;
	}

	return 0;
}

static char *uwsgi_lua_code_string(char *id, char *code, char *func, char *key, uint16_t keylen) {

	static struct lua_State *L = NULL;

	if (!L) {
		L = luaL_newstate();
                luaL_openlibs(L);
                if (luaL_loadfile(L, code) || lua_pcall(L, 0, 0, 0)) {
                ulua_log("unable to load file %s: %s", code, lua_tostring(L, -1));
			lua_close(L);
			L = NULL;
			return NULL;
                }
		lua_getglobal(L, func);
		if (!lua_isfunction(L,-1)) {
			ulua_log("unable to find %s function in lua file %s", func, code);
			lua_close(L);
			L = NULL;
			return NULL;
		}
		lua_pushnil(L);
	}


	lua_pop(L, 1);

	lua_pushvalue(L, -1);
	lua_pushlstring(L, key, keylen);

#ifdef UWSGI_DEBUG
	ulua_log("stack pos %d %.*s", lua_gettop(L), keylen, key);
#endif

        if (lua_pcall(L, 1, 1, 0)) {
                ulua_log("error running function `f': %s", lua_tostring(L, -1));
                return NULL;

        }

	if (lua_isstring(L, -1)) {
                const char *ret = lua_tostring(L, -1);
		return (char *)ret;
        }

        return NULL;
}

static int uwsgi_lua_signal_handler(uint8_t sig, void *handler) {

	lua_State *L = NULL;

	if (uwsgi.mywid == 0) {
		if (!uwsgi.muleid) {
			return -1;
		}

		L = ULUA_MULE_STATE;

		if (!L) { // mule without lua_State
			ulua_log("signal: can't handle signal on mule%d without lua_State", uwsgi.muleid);
			return -1;
		}

	} else {

		if (!ulua.state) {
			ulua_log("signal: can't handle signal on worker%d without lua_State(s)", uwsgi.mywid);
			return -1;
		}

		struct wsgi_request *wsgi_req = current_wsgi_req();

		L = ULUA_WORKER_STATE[wsgi_req->async_id];

#ifdef UWSGI_DEBUG
		ulua_log("managing signal handler on core %d", wsgi_req->async_id);
#endif
	}

	int type;

	lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_SIGNAL_REF);
	lua_pushnumber(L, sig);
	lua_gettable(L, -2);

	type = lua_type(L, -1);

	if (!(type == LUA_TFUNCTION) && !((type == LUA_TTABLE || type == LUA_TUSERDATA) && uwsgi_lua_metatable_call(L, -1))) {
		ulua_log("signal: attempt to call a %s value", lua_typename(L, type));
		lua_pop(L, 2);
		return -1;
	}

	lua_pushnumber(L, sig);

	if (lua_pcall(L, 1 + (type == LUA_TTABLE), 0, 0)) {
		ulua_log("signal: error running signal function: %s", lua_tostring(L, -1));
		lua_pop(L, 2);
		return -1;
	}

	lua_pop(L, 2);
	return 0;
}

static uint64_t uwsgi_lua_rpc(void * func, uint8_t argc, char **argv, uint16_t argvs[], char **buffer) {

	uint8_t i;
	const char *sv;
	size_t sl;

	int type;
	struct wsgi_request *wsgi_req = current_wsgi_req();

	lua_State *L = ULUA_WORKER_STATE[wsgi_req->async_id];
	lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_RPC_REF);

	lua_getfield(L, -1, (const char *) func);

	type = lua_type(L, -1);

	if (!(type == LUA_TFUNCTION) && !((type == LUA_TTABLE || type == LUA_TUSERDATA) && uwsgi_lua_metatable_call(L, -1))) {
		ulua_log("rpc: attempt to call a %s value", lua_typename(L, type));
		lua_pop(L, 2);
		return 0;
	}

	for(i = 0; i < argc; i++) {
		lua_pushlstring(L, argv[i], argvs[i]);
	}

	if (lua_pcall(L, argc + (type == LUA_TTABLE), 1, 0)) {
		ulua_log("rpc: error running rpc function: %s", lua_tostring(L, -1));
		lua_pop(L, 2);
		return 0;
	}

	if (uwsgi_lua_isutable(L, -1)) {
		uwsgi_lua_metatable_tostring_protected(L, -1);
	}

	sv = lua_tolstring(L, -1, &sl);

	if (sl > 0) {
		*buffer = uwsgi_malloc(sizeof(char) * sl);
		memcpy(*buffer, sv, sizeof(char) * sl);
	}

	lua_pop(L, 2);

	return sl;
}

static void uwsgi_lua_configurator_array(lua_State *L) {

	int i;
	int n = lua_rawlen(L, -3);

	for(i=1;i<=n;i++) {
		lua_rawgeti(L, 1, i);
		if (lua_istable(L, -1)) {
                	lua_pushnil(L);
                        while (lua_next(L, -2) != 0) {
                        	char *key = uwsgi_str((char *)lua_tostring(L, -2));
                                char *value = uwsgi_str((char *)lua_tostring(L, -1));
                                add_exported_option(key, value, 0);
                                lua_pop(L, 1);
                        }
                }
	}
}


static void uwsgi_lua_configurator(char *filename, char *magic_table[]) {
	size_t len = 0;
	uwsgi_log_initial("[uWSGI] getting Lua configuration from %s\n", filename);
	char *code = uwsgi_open_and_read(filename, &len, 1, magic_table);
	lua_State *L = luaL_newstate();
	if (!L) {
		uwsgi_log("unable to initialize Lua state for configuration\n");
		exit(1);
	}
        luaL_openlibs(L);
	if (luaL_dostring(L, code) != 0) {
		uwsgi_log("error running Lua configurator: %s\n", lua_tostring(L, -1));
		exit(1);
	}
	free(code);

	if (!lua_istable(L, -1)) {
		uwsgi_log("Lua configurator has to return a table !!!\n");
		exit(1);
	}

	lua_pushnil(L);
	// we always use uwsgi_str to avoid GC destroying our strings
	// and to be able to call lua_close at the end
	while (lua_next(L, -2) != 0) {
		// array ?
		if (lua_isnumber(L, -2)) {
			uwsgi_lua_configurator_array(L);
			break;
		}
		// dictionary
		else {
			char *key = uwsgi_str((char *)lua_tostring(L, -2));
			if (lua_istable(L, -1)) {
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					char *value = uwsgi_str((char *)lua_tostring(L, -1));
					add_exported_option(key, value, 0);
					lua_pop(L, 1);
				}
			}
			else {
				char *value = uwsgi_str((char *)lua_tostring(L, -1));
				add_exported_option(key, value, 0);
			}
		}
		lua_pop(L, 1);
	}

	// this will destroy the whole Lua state
	lua_close(L);
}

static void uwsgi_register_lua_features() {
	uwsgi_register_configurator(".luac", uwsgi_lua_configurator);
	uwsgi_register_configurator(".lua", uwsgi_lua_configurator);

	// non zero or non inited defaults:
	ulua.gc_freq = ULUA_OPTDEF_GC_FREQ;
}

static void uwsgi_lua_hijack(void) {

	if (ulua.shell && uwsgi.mywid == 1) {

		if (ulua.shell_oneshot && uwsgi.workers[uwsgi.mywid].hijacked_count > 0) {
			uwsgi.workers[uwsgi.mywid].hijacked = 0;
			return;
		}

		uwsgi.workers[uwsgi.mywid].hijacked = 1;
		uwsgi.workers[uwsgi.mywid].hijacked_count++;
		// re-map stdin to stdout and stderr if we are logging to a file
		if (uwsgi.logfile) {
			if (dup2(0, 1) < 0) {
				uwsgi_error("dup2()");
			}
			if (dup2(0, 2) < 0) {
				uwsgi_error("dup2()");
			}
		}

		// run in the first state
		lua_State *L = ULUA_WORKER_STATE[0];
		lua_getglobal(L, "debug");
		lua_getfield(L, -1, "debug");

		ulua_log("Hallo, this is lua debug.debug() aka lua_debug, use Control+D to %s",
			(ulua.shell_oneshot || uwsgi.master_process) ? "resume" : "exit");

		if (lua_pcall(L, 0, 0, 0)) {
			ulua_log("unable to call 'debug.debug()': %s", lua_tostring(L, -1));
		}

		if (ulua.shell_oneshot || uwsgi.master_process) { // master will respawn it anyway

			uwsgi.workers[uwsgi.mywid].hijacked = 0;

			uwsgi_log("\n");
			ulua_log("worker %d has been resumed...", uwsgi.mywid);

			return;
		}

		exit(UWSGI_QUIET_CODE);

	}
}


static void uwsgi_lua_init_apps() {

	if (!ULUA_WORKER_ANYAPP) {
		return;
	}

	int i,j,sid;

	//cores per lua thread
	int cores = uwsgi.threads > 1 ? 1 : uwsgi.cores;

	if (uwsgi.mywid > 0) {	// lazy app
		sid = ULUA_MYWID*uwsgi.threads;

		for(i=0;i<uwsgi.threads;i++) {
			uwsgi_lua_init_state(&(ULUA_WORKER_STATE[i]), uwsgi.mywid, sid + i + 1, cores);
		}

		ulua_log("inited %d lua_State(s) for worker %d", uwsgi.threads, uwsgi.mywid);
	} else {
		for(j=0;j<uwsgi.numproc;j++){
			sid = j*uwsgi.threads;

			for(i=0;i<uwsgi.threads;i++) {
				uwsgi_lua_init_state(&(ulua.state[j][i]), j + 1, sid + i + 1, cores);
			}

			ulua_log("inited %d lua_State(s) for worker %d", uwsgi.threads, j + 1);
		}
	}

	if (ulua.wsapi) {
		uwsgi_apps_cnt++;
	}
}

static int uwsgi_lua_mule_msg(char *msg, size_t len) {

	lua_State *L = ULUA_MULE_STATE;

	if (!L) {
		return 0;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, ULUA_MULE_MSG_REF);

	int type = lua_type(L, -1);

	if (!(type == LUA_TFUNCTION) && !((type == LUA_TTABLE || type == LUA_TUSERDATA) && uwsgi_lua_metatable_call(L, -1))) {
		lua_pop(L, 1);
		return 0;
	}

	lua_pushlstring(L, msg, len);

	if (lua_pcall(L, 1 + (type == LUA_TTABLE), 0, 0)) {
		ulua_log("mule%d: error running msg hook function: %s", uwsgi.muleid, lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	return 1;
}

static int uwsgi_lua_mule(char *file) {

	int type;
	lua_State *L;
	char *load;

	if (!uwsgi_endswith(file, ".lua") && !uwsgi_endswith(file, ".luac")) {
		return 0;
	}

	load = strchr(file, '@');

	if (load) {
		*(load++) = 0;
	} else {
		load = file;
	}

	ULUA_MULE_STATE = luaL_newstate();
	L = ULUA_MULE_STATE;

	luaL_openlibs(L);

	uwsgi_lua_api_newglobaltable(L, "uwsgi");
	uwsgi_lua_api_push(L, -1, uwsgi_api_base);
	uwsgi_lua_api_push(L, -1, uwsgi_api_mule);

	lua_pushstring(L, (file != load) ? file : "");
	lua_setfield(L, -2, "mule_param");

	lua_pushstring(L, UWSGI_VERSION);
	lua_setfield(L, -2, "version");

	// reserve ref 1 for mule_msg_hook func
	lua_pushboolean(L, 0);
	luaL_ref(L, LUA_REGISTRYINDEX);

	// signal table ref 2
	lua_newtable(L);
	lua_pushvalue(L, -1);

	luaL_ref(L, LUA_REGISTRYINDEX);
	lua_setfield(L, -2, "signal_ref");

	lua_pop(L, 1);

	if (luaL_loadfile(L, load) || lua_pcall(L, 0, 1, 0)) {
		ulua_log("mule%d: unable to load Lua file %s: %s", uwsgi.muleid, load, lua_tostring(L, -1));

		// init error close the state
		lua_close(L);
		ULUA_MULE_STATE = NULL;
		return 0;
	}

	// cleanup the mess
	lua_gc(L, LUA_GCCOLLECT, 0);

	for (;;) {
		type = lua_type(L, -1);

		if (!(type == LUA_TFUNCTION) && !((type == LUA_TTABLE || type == LUA_TUSERDATA) && uwsgi_lua_metatable_call(L, -1))) {
			lua_pop(L, 1);
			break;
		}

		if (lua_pcall(L, (type == LUA_TTABLE), 1, 0)) {
			ulua_log("mule%d: error running loop function: %s", uwsgi.muleid, lua_tostring(L, -1));

			// loop exeption close the state
			lua_close(L);
			ULUA_MULE_STATE = NULL;
			return 1; // respawn pls
		}
	}

	return 0;
}

struct uwsgi_plugin lua_plugin = {

	.name = "lua",
	.modifier1 = 6,

	.init = uwsgi_lua_init,
	.options = uwsgi_lua_options,
	.request = uwsgi_lua_request,
	.after_request = uwsgi_lua_after_request,

	.init_apps = uwsgi_lua_init_apps,

	.mule = uwsgi_lua_mule,
	.mule_msg = uwsgi_lua_mule_msg,

	.magic = uwsgi_lua_magic,
	.signal_handler = uwsgi_lua_signal_handler,

	.hijack_worker = uwsgi_lua_hijack,

	.code_string = uwsgi_lua_code_string,
	.rpc = uwsgi_lua_rpc,

	.on_load = uwsgi_register_lua_features,
};

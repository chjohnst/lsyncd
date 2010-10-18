#include "config.h"
#define LUA_USE_APICHECK 1

#ifdef HAVE_SYS_INOTIFY_H
#  include <sys/inotify.h>
#else
#  include "inotify-nosys.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/**
 * The inotify file descriptor.
 */
static int inotify_fd;

/**
 * TODO allow configure.
 */
const uint32_t standard_event_mask =
		IN_ATTRIB   | IN_CLOSE_WRITE | IN_CREATE     |
		IN_DELETE   | IN_DELETE_SELF | IN_MOVED_FROM |
		IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR;


/**
 * Set to TERM or HUP in signal handler, when lsyncd should end or reset ASAP.
 */
volatile sig_atomic_t reset = 0;

/**
 * "secured" calloc.
 */
void *
s_calloc(size_t nmemb, size_t size)
{
	void *r = calloc(nmemb, size);
	if (r == NULL) {
		printf("Out of memory!\n");
		exit(-1); // ERRNO 
	}
	return r;
}

/**
 * "secured" malloc. the deamon shall kill itself
 * in case of out of memory.
 */
void *
s_malloc(size_t size)
{
	void *r = malloc(size);
	if (r == NULL) {
		printf("Out of memory!\n");
		exit(-1);  // ERRNO
	}
	return r;
}

/*****************************************************************************
 * Library calls for lsyncd.lua
 * 
 * These are as minimal as possible glues to the operating system needed for 
 * lsyncd operation.
 *
 ****************************************************************************/

/**
 * Adds an inotify watch
 * 
 * @param dir (Lua stack) path to directory
 * @return    (Lua stack) numeric watch descriptor
 */
static int
l_add_watch(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	lua_Integer wd = inotify_add_watch(inotify_fd, path, standard_event_mask);
	lua_pushinteger(L, wd);
	return 1;
}

/**
 * Executes a subprocess. Does not wait for it to return.
 * 
 * @param  (Lua stack) Path to binary to call
 * @params (Lua stack) list of string as arguments
 * @return (Lua stack) the pid on success, 0 on failure.
 */
static int
l_exec(lua_State *L)
{
	const char *binary = luaL_checkstring(L, 1);
	int argc = lua_gettop(L) - 1;
	pid_t pid;
	int i;
	char const **argv = s_calloc(argc + 2, sizeof(char *));

	argv[0] = binary;
	for(i = 1; i <= argc; i++) {
		argv[i] = luaL_checkstring(L, i + 1);
	}
	argv[i] = NULL;

	pid = fork();

	if (pid == 0) {
		//if (!log->flag_nodaemon && log->logfile) {
		//	if (!freopen(log->logfile, "a", stdout)) {
		//		printlogf(log, ERROR, "cannot redirect stdout to [%s].", log->logfile);
		//	}
		//	if (!freopen(log->logfile, "a", stderr)) {
		//		printlogf(log, ERROR, "cannot redirect stderr to [%s].", log->logfile);
		//	}
		//}
		execv(binary, (char **)argv);
		// in a sane world execv does not return!
		printf("Failed executing [%s]!\n", binary);
		exit(-1); // ERRNO
	}

	free(argv);
	lua_pushnumber(L, pid);
	return 1;
}


/**
 * Converts a relative directory path to an absolute.
 * 
 * @param dir a relative path to directory
 * @return    absolute path of directory
 */
static int
l_real_dir(lua_State *L)
{
	luaL_Buffer b;
	char *cbuf;
	const char *rdir = luaL_checkstring(L, 1);
	
	/* use c-library to get absolute path */
	cbuf = realpath(rdir, NULL);
	if (cbuf == NULL) {
		printf("failure getting absolute path of \"%s\"\n", rdir);
		return 0;
	}
	{
		/* makes sure its a directory */
	    struct stat st;
	    stat(cbuf, &st);
	    if (!S_ISDIR(st.st_mode)) {
			printf("failure in real_dir \"%s\" is not a directory\n", rdir);
			free(cbuf);
			return 0;
	    }
	}

	/* returns absolute path with a concated '/' */
	luaL_buffinit(L, &b);
	luaL_addstring(&b, cbuf);
	luaL_addchar(&b, '/');
	luaL_pushresult(&b);
	free(cbuf);
	return 1;
}

/**
 * Dumps the LUA stack. For debugging purposes.
 */
static int
l_stackdump(lua_State* L)
{
	int i;
	int top = lua_gettop(L);
	printf("total in stack %d\n",top);
	for (i = 1; i <= top; i++) { 
		int t = lua_type(L, i);
		switch (t) {
			case LUA_TSTRING:
				printf("%d string: '%s'\n", i, lua_tostring(L, i));
				break;
			case LUA_TBOOLEAN:
				printf("%d boolean %s\n", i, lua_toboolean(L, i) ? "true" : "false");
				break;
			case LUA_TNUMBER: 
				printf("%d number: %g\n", i, lua_tonumber(L, i));
				break;
			default:  /* other values */
				printf("%d %s\n", i, lua_typename(L, t));
				break;
		}
	}
	
	printf("\n");
	return 0;
}

/**
 * Reads the directories sub directories.
 * 
 * @param  (Lua stack) absolute path to directory.
 * @return (Lua stack) a table of directory names.
 */
static int
l_sub_dirs (lua_State *L)
{
	const char * dirname = luaL_checkstring(L, 1);
	DIR *d;
	int idx = 1;

	d = opendir(dirname);
	if (d == NULL) {
		printf("cannot open dir %s.\n", dirname);
		return 0;
	}
	
	lua_newtable(L);
	while (!reset) {
		struct dirent *de = readdir(d);
		bool isdir;
		if (de == NULL) {
			/* finished */
			break;
		}
		if (de->d_type == DT_UNKNOWN) {
			/* must call stat on some systems :-/ */
			char *subdir = s_malloc(strlen(dirname) + strlen(de->d_name) + 2);
			struct stat st;
			strcpy(subdir, dirname);
			strcat(subdir, "/");
			strcat(subdir, de->d_name);
			stat(subdir, &st);
			isdir = S_ISDIR(st.st_mode);
			free(subdir);
		} else {
			/* we can trust readdir */
			isdir = de->d_type == DT_DIR;
		}
		if (!isdir || !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			/* ignore non directories and . and .. */
			continue;
		}

		/* add this to the Lua table */
		lua_pushnumber(L, idx++);
		lua_pushstring(L, de->d_name);
		lua_settable(L, -3);
	}
	return 1;
}

/*****************************************************************************
 * Lsyncd Core 
 ****************************************************************************/

static const luaL_reg lsyncdlib[] = {
		{"add_watch", l_add_watch},
		{"exec",      l_exec},
		{"real_dir",  l_real_dir},
		{"stackdump", l_stackdump},
		{"sub_dirs",  l_sub_dirs},
		{NULL, NULL}
};

/**
 * Waits after startup for all children.
 * 
 * @param (Lua stack) a table of the children pids.
 */
void
wait_startup(lua_State *L) 
{
	/* wait for all children spawned in startup */
	int pidn, *pids, i;
	if (lua_type(L, 1) == LUA_TNIL) {
		printf("Lua function startup did not return a pidtable!\n");
		exit(-1); // ERRNO
	}
	pidn = lua_objlen (L, -1);
	if (pidn == 0) {
		/* nothing to do on zero pids */
		return;
	}
	pids = s_calloc(pidn, sizeof(int));
	for(i = 0; i < pidn; i++) {
		// TODO
	}
	free(pids);
}


/**
 * Main
 */
int
main(int argc, char *argv[])
{
	/* the Lua interpreter */
	lua_State* L;

	/* load Lua */
	L = lua_open();
	luaL_openlibs(L);
	luaL_register(L, "lsyncd", lsyncdlib);
	lua_setglobal(L, "lysncd");

	if (luaL_loadfile(L, "lsyncd.lua")) {
		printf("error loading lsyncd.lua: %s\n", lua_tostring(L, -1));
		return -1; // ERRNO
	}
	if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
		printf("error running lsyncd.lua: %s\n", lua_tostring(L, -1));
		return -1; // ERRNO
	}
	
	if (luaL_loadfile(L, "lsyncd-conf.lua")) {
		printf("error loading lsyncd-conf.lua: %s\n", lua_tostring(L, -1));
		return -1; // ERRNO
	}
	if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
		printf("error running lsyncd-conf.lua: %s\n", lua_tostring(L, -1));
		return -1; // ERRNO
	}

	/* open inotify */
	inotify_fd = inotify_init();
	if (inotify_fd == -1) {
		printf("Cannot create inotify instance! (%d:%s)\n", errno, strerror(errno));
		return -1; // ERRNO
	}

	/* initialize */
	/* lua code will set configuration and add watches */
	lua_getglobal(L, "lsyncd_initialize");
	lua_call(L, 0, 0);
	
	/* startup */
	/* lua code will perform startup calls like recursive rsync */
	lua_getglobal(L, "startup");
	lua_call(L, 0, 1);
	wait_startup(L);

	/* cleanup */
	close(inotify_fd);
	lua_close(L);
	return 0;
}

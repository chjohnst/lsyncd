#!/usr/bin/lua
require("posix")
dofile("tests/testlib.lua")

cwriteln("****************************************************************")
cwriteln(" Testing excludes                                               ")
cwriteln("****************************************************************")

local tdir, srcdir, trgdir = mktemps()
local logfile = tdir .. "log"
local cfgfile = tdir .. "config.lua"
local range = 5
local log = {"-log", "all"}

writefile(cfgfile, [[
settings = {
    logfile = "]]..logfile..[[",
    nodaemon = true,
	delay = 3,
}

sync {
    default.rsync,
	source = "]]..srcdir..[[",
	target = "]]..trgdir..[[",
	exclude = {
        "erf",
		"/eaf",
		"erd/",
		"/ead",
	},
}]]);

-- writes all files
local function writefiles() 
	posix.mkdir(srcdir .. "d");
	writefile(srcdir .. "erf", "erf");
	writefile(srcdir .. "eaf", "erf");
	writefile(srcdir .. "erd", "erd");
	writefile(srcdir .. "ead", "ead");
	writefile(srcdir .. "d/erf", "erf");
	writefile(srcdir .. "d/eaf", "erf");
	writefile(srcdir .. "d/erd", "erd");
	writefile(srcdir .. "d/ead", "ead");
end

-- test if the filename exists, fails if this is different to expect
local function testfile(filename, expect) 
	local stat, err = posix.stat(filename)
	if stat and not expect then
		cwriteln("failure: ",filename," should be excluded");
		os.exit(1);
	end
	if not stat and expect then
		cwriteln("failure: ",filename," should not be excluded");
		os.exit(1);
	end
end

-- test all files
local function testfiles() 
	testfile(srcdir .. "erf", false);
	testfile(srcdir .. "eaf", false);
	testfile(srcdir .. "erd", true);
	testfile(srcdir .. "ead", true);
	testfile(srcdir .. "d/erf", false);
	testfile(srcdir .. "d/eaf", true);
	testfile(srcdir .. "d/erd", true);
	testfile(srcdir .. "d/ead", true);
end


cwriteln("testing startup excludes");
writefiles();
cwriteln("starting Lsyncd");
local pid = spawn("./lsyncd", cfgfile);
cwriteln("waiting for Lsyncd to start");
posix.sleep(3)
cwriteln("testing excludes after startup");
testfiles();
cwriteln("ok, removing sources");
if srcdir:sub(1,4) ~= "/tmp" then
	-- just to make sure before rm -rf
	cwriteln("exist before drama, srcdir is '", srcdir, "'");
	os.exit(1);
end
os.execute("rm -rf "..srcdir.."/*");
writeln("waiting for Lsyncd to remove destination");
if os.execute("diff -urN "..srcdir.." "..trgdir) ~= 0 then
	os.exit(1);
end

posix.sleep(5);
writeln("writing files after startup");
writefiles();
writeln("waiting for Lsyncd to transmit changes");
posix.sleep(5);
testfiles();

writeln("killing started Lsyncd");
posix.kill(pid);
local _, exitmsg, lexitcode = posix.wait(lpid);
cwriteln("Exitcode of Lsyncd = ", exitmsg, " ", lexitcode);
posix.sleep(1);
if lexitcode == 0 then
	cwriteln("OK");
end
os.exit(lexitcode);

-- TODO remove temp

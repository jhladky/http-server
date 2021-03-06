#!/usr/bin/python

import argparse
import subprocess
import sys
import os
import signal
import platform

success = True

def close_server(uut):
    os.kill(uut.pid, signal.SIGINT)
    resLine = uut.stdout.readline()
    return resLine == "Server exiting cleanly.\n"

def wdribble(port, f):
    return ["support/wdribble-Darwin-i386", "0", port,
            "1", "0", "0", "127.0.0.1", f];

def print_result(test):
    print("...." + ("PASSED" if test else "FAILED"))
    return test

parser = argparse.ArgumentParser();
parser.add_argument("prog_dir",
                    help="The directory containing the project source files.")
args = parser.parse_args()
nothing = open("/dev/null", "w");

os.chdir(args.prog_dir)

sys.stdout.write("Compiling program")
subprocess.call(["make", "clean"], stdout=nothing)
if not (print_result(not subprocess.call(["make", "all"], stdout=nothing))):
    success = False
    sys.exit()

binName = "json-server-" + platform.system() + "-" + platform.machine()

errorLog = open("error.log", "a")
uut = subprocess.Popen(
    ["./" + binName],
    stdout=subprocess.PIPE,
    stdin=subprocess.PIPE,
    stderr=errorLog
)

declLine = uut.stdout.readline()
uutPort = int(declLine.split(" ")[-1])

sys.stdout.write(declLine)
print("Port: " + str(uutPort))

sys.stdout.write("Testing if server exits cleanly")

if not print_result(close_server(uut)):
    success = False

uut.wait()
nothing.close()
errorLog.close()

if success:
    print("SUCCESS :)")
else:
    print("FAIL :(")

# parser.add_argument("ref_port", help="the port of the server to be tested");
# parser.add_argument("uut_port", help="the port of the server to be tested");
# args = parser.parse_args();

# ref = ["./wdribble-Darwin-i386", "0", args.ref_port,
#        "1", "0", "0", "127.0.0.1", "/big_file.iso"];
# uut = ["./wdribble-Darwin-i386", "0", args.uut_port,
#        "1", "0", "0", "127.0.0.1", "/big_file.iso"];
# tests = [("/", "index"),
#          ("/asdfg", "404"),
#          ("/no.html", "403"),
#          ("/f", "listing")];

# processes = [];
# nothing = open('/dev/null', 'w');

# print("Testing Errors");
# for path, name in tests:
#     subprocess.call("./wdribble-Darwin-i386 0 " + args.ref_port + " 1 0 0 127.0.0.1 " + path + " > " + name + ".out 2> /dev/null", shell=True);
#     subprocess.call("./wdribble-Darwin-i386 0 " + args.uut_port + " 1 0 0 127.0.0.1 " + path + " > " + name + ".myout 2> /dev/null", shell=True);

#     try:
#         subprocess.check_output(["diff", name + ".out", name + ".myout"]);
#     except subprocess.CalledProcessError as error:
#                 print("Failed " + name + " test with diff return code " + str(error.returncode));

# print("Testing Concurrency");
# processes.append(subprocess.Popen(uut, stdout=nothing));

# uut[-1] = "/";

# for _ in range(100):
#     processes.append(subprocess.Popen(uut, stdout=nothing, stderr=nothing));

# print("Waiting for processes");
# for process in processes:
#     process.wait();

# print("Testing CGI Quit")
# uut[-1] = "/cgi-bin/quit";
# processes.append(subprocess.Popen(uut));
# uut[-1] = "/cgi-bin/quit?confirm=1";
# processes.append(subprocess.Popen(uut));

# print("Finished!");

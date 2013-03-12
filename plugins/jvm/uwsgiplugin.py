import os,sys

NAME='jvm'

JVM_INCPATH = None
JVM_LIBPATH = None

operating_system = os.uname()[0].lower()
arch = os.uname()[4]

if arch in ('i686', 'x86', 'x86_32'):
    arch = 'i386'
elif arch in ('x86_64',):
    arch = 'amd64'

# try to detect the JVM
if operating_system == 'darwin':
    known_jvms = ('/System/Library/Frameworks/JavaVM.framework/Headers',)
    for jvm in known_jvms:
        if os.path.exists(jvm):
            JVM_INCPATH = ["-Wno-deprecated-declarations", "-I%s" % jvm]
            JVM_LIBPATH = ["-framework JavaVM"]
else:
    known_jvms = ('/usr/lib/jvm/java-7-openjdk', '/usr/local/openjdk7', '/usr/lib/jvm/java-6-openjdk', '/usr/local/openjdk', '/usr/java', '/usr/lib/jvm/java/')
    for jvm in known_jvms:
       if os.path.exists(jvm + '/include'):
           JVM_INCPATH = ["-I%s/include/" % jvm, "-I%s/include/%s" % (jvm, operating_system)]
           JVM_LIBPATH = ["-L%s/jre/lib/%s/server" % (jvm, arch)]
           break
       if os.path.exists("%s-%s/include" % (jvm, arch)):
           jvm = "%s-%s" % (jvm, arch)
           JVM_INCPATH = ["-I%s/include/" % jvm, "-I%s/include/%s" % (jvm, operating_system)]
           JVM_LIBPATH = ["-L%s/jre/lib/%s/server" % (jvm, arch)]
           break

try: 
    JVM_INCPATH = os.environ['UWSGICONFIG_JVM_INCPATH'] 
except: 
    pass 

try: 
    JVM_LIBPATH = os.environ['UWSGICONFIG_JVM_LIBPATH'] 
except: 
    pass 

if not JVM_INCPATH or not JVM_LIBPATH:
    print("unable to autodetect the JVM path, please specify UWSGICONFIG_JVM_INCPATH and UWSGICONFIG_JVM_LIBPATH environment vars")
    os._exit(1)

CFLAGS = JVM_INCPATH
LDFLAGS = JVM_LIBPATH
LIBS = ['-ljvm']
if "-framework JavaVM" in JVM_LIBPATH:
    LIBS = []

GCC_LIST = ['jvm_plugin']

if 'LD_RUN_PATH' in os.environ:
    os.environ['LD_RUN_PATH'] += ':' + JVM_LIBPATH[0][2:]
else:
    os.environ['LD_RUN_PATH'] = JVM_LIBPATH[0][2:]

def post_build(config):
    if os.system("javac %s/plugins/jvm/uwsgi.java" % os.getcwd()) != 0:
        os._exit(1)
    if os.system("cd %s/plugins/jvm ; jar cvf uwsgi.jar *.class" % os.getcwd()) != 0:
        os._exit(1)
    print("*** uwsgi.jar available in %s/plugins/jvm/uwsgi.jar ***" % os.getcwd())

import os
import sys
try:
    from distutils import sysconfig
    paths = [
        sysconfig.get_python_inc(),
        sysconfig.get_python_inc(plat_specific=True),
    ]
except ImportError:
    import sysconfig
    paths = [
        sysconfig.get_path('include'),
        sysconfig.get_path('platinclude'),
    ]

os.environ['UWSGI_PYTHON_NOLIB'] = '1'

NAME = 'pyuwsgi'

CFLAGS = ['-I' + path for path in paths]
LDFLAGS = []
LIBS = []

PY3 = sys.version_info[0] >= 3
if not PY3:
    GCC_LIST = ['pyuwsgi']
else:
    GCC_LIST = []

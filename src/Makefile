##
##  Makefile -- Build procedure for sample fastdfs Apache module
##  Autogenerated via ``apxs -n fastdfs -g''.
##

APACHE_BASE_PATH=/home/ycdev/work/httpd-2.2.27

builddir=.
top_srcdir=$(APACHE_BASE_PATH)
top_builddir=$(APACHE_BASE_PATH)
include $(APACHE_BASE_PATH)/build/special.mk

#   the used tools
APXS=/usr/sbin/apxs
APACHECTL=/usr/sbin/apachectl

#   additional defines, includes and libraries
CFLAGS=-Wall
DEFS=-D_FILE_OFFSET_BITS=64 -DFDFS_OUTPUT_CHUNK_SIZE='256*1024' -DFDFS_MOD_CONF_FILENAME='"/etc/fdfs/mod_fastdfs.conf"'
INCLUDES=-I/usr/include/fastdfs -I/usr/include/fastcommon/
LIBS= -lfastcommon -lfdfsclient
SH_LIBS=-lfastcommon -lfdfsclient

#   the default target
all: local-shared-build

#   install the shared object file into Apache 
install: install-modules-yes
	if [ ! -f /etc/fdfs/mod_fastdfs.conf ]; then cp mod_fastdfs.conf /etc/fdfs/; fi
#   cleanup
clean:
	-rm -f mod_fastdfs.o mod_fastdfs.lo mod_fastdfs.slo mod_fastdfs.la 

#   simple test
test: reload
	lynx -mime_header http://localhost/fastdfs

#   install and activate shared object by reloading Apache to
#   force a reload of the shared object file
reload: install restart

#   the general Apache start/restart/stop
#   procedures
start:
	$(APACHECTL) start
restart:
	$(APACHECTL) restart
stop:
	$(APACHECTL) stop


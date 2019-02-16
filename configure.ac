# get rid of that stupid cache mechanism
rm -f config.cache

m4_define([v_maj], [0])
m4_define([v_min], [0])
m4_define([v_mic], [1])
m4_define([v_ver], [v_maj.v_min.v_mic])

m4_define([lt_cur], m4_eval(v_maj + v_min))
m4_define([lt_rev], v_mic)
m4_define([lt_age], v_min)

AC_INIT([gst-bt], [v_ver], [enesim-devel@googlegroups.com])
AC_PREREQ([2.60])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_USE_SYSTEM_EXTENSIONS

AM_INIT_AUTOMAKE([foreign 1.6 dist-bzip2])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_USE_SYSTEM_EXTENSIONS

LT_PREREQ([2.2])
LT_INIT([win32-dll])

version_info="lt_cur:lt_rev:lt_age"
AC_SUBST([version_info])
AC_DEFINE_UNQUOTED([VERSION_MAJOR], [v_maj], [Major version])
AC_DEFINE_UNQUOTED([VERSION_MINOR], [v_min], [Minor version])
AC_DEFINE_UNQUOTED([VERSION_MICRO], [v_mic], [Micro version])

VMAJ=v_maj
AC_SUBST([VMAJ])

### Needed information

AC_CANONICAL_HOST

### Checks for programs

AC_PROG_CC
AC_PROG_CXX

# pkg-config
PKG_PROG_PKG_CONFIG

# Check whether pkg-config supports Requires.private
if ! $PKG_CONFIG --atleast-pkgconfig-version 0.22; then
   AC_MSG_ERROR([pkg-config >= 0.22 is required])
fi

### Checks for libraries
gst_bt_requirements_cflags=""
gst_bt_requirements_libs=""
gst_bt_requirements_pc="libtorrent-rasterbar"


gstreamer_api_default="0.10"
AC_ARG_WITH(gstreamer-api,
   AS_HELP_STRING([--with-gstreamer-api],
       [manually set the gstreamer API version 0.10 or 1.0 are valid values]),
       [gstreamer_api="${withval}"])

if test "x$gstreamer_api" = "x" ; then
  gstreamer_api="${gstreamer_api_default}"
fi

case "$gstreamer_api" in
  1.0)
  gst_bt_requirements_pc+=" gstreamer-1.0 gstreamer-base-1.0"
  AC_DEFINE_UNQUOTED(HAVE_GST_1, [1], [Use GStreamer 1.0])
  ;;
  *)
  AC_MSG_ERROR([Unsupported gstreamer version "$gstreamer_api"])
  ;;
esac

dnl make GST_MAJORMINOR available in Makefile.am
GST_MAJORMINOR=$gstreamer_api
AC_SUBST(GST_MAJORMINOR)

PKG_CHECK_MODULES([GST_BT], [${gst_bt_requirements_pc}])

GST_BT_LIBS="${GST_BT_LIBS} ${gst_bt_requirements_libs}"
GST_BT_CFLAGS="${GST_BT_CFLAGS} ${gst_bt_requirements_cflags}"

## Make the debug preprocessor configurable

AC_CONFIG_FILES([
Makefile
])

AC_OUTPUT

#####################################################################
## Info

echo
echo
echo
echo "------------------------------------------------------------------------"
echo "$PACKAGE $VERSION"
echo "------------------------------------------------------------------------"
echo "Compilation.................................: make (or gmake)"
echo "  CPPFLAGS..................................: $CPPFLAGS"
echo "  CFLAGS....................................: $CFLAGS"
echo "  LDFLAGS...................................: $LDFLAGS"
echo "  GStreamer API.............................: $gstreamer_api"
echo
echo "Installation................................: make install (as root if needed, with 'su' or 'sudo')"
echo "  prefix....................................: $prefix"
echo


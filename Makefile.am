include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_audio_fork

mod_LTLIBRARIES = mod_audio_fork.la
# ★★★ ws_uri.cpp MUST be listed. It was split out of lws_glue.cpp when the URI
#   parser was rewritten, and this file was never updated — so the autotools
#   build has been failing since then with undefined references to
#   ws_uri_scheme / ws_uri_parse_authority. `-no-undefined` turns that into a
#   hard link error rather than a surprise at module load.
#   ⚠ CMakeLists.txt is the path everything actually uses, which is exactly why
#     this one rotted unnoticed. Adding a file means adding it in both places.
mod_audio_fork_la_SOURCES  = mod_audio_fork.c lws_glue.cpp parser.cpp audio_pipe.cpp ws_uri.cpp
mod_audio_fork_la_CFLAGS   = $(AM_CFLAGS) -Wall -Wextra
# ★ C++14 to match CMakeLists.txt. The two build paths disagreeing on the
#   language standard is its own trap: code that compiles under one silently
#   fails under the other.
mod_audio_fork_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++14 -Wall -Wextra

mod_audio_fork_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
# ★ speexdsp was missing too — lws_glue.cpp calls speex_resampler_* directly.
mod_audio_fork_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs libwebsockets speexdsp` 

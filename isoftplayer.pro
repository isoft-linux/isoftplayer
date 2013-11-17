######################################################################
# Automatically generated by qmake (3.0) ?? 11? 5 22:25:19 2013
######################################################################
cache()

system(which clang++ 1>/dev/null 2>&1) {
    QMAKE_CXX = clang++
}

TEMPLATE = app
TARGET = isoftplayer
QT_CONFIG -= no-pkg-config
CONFIG += qt debug link_pkgconfig 
QT += gui core widgets opengl
INCLUDEPATH += .

QMAKE_CXXFLAGS += -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS

unix|macx {
	CONFIG += link_pkgconfig
	PKGCONFIG += libavcodec libavutil libswresample libavformat libswscale sdl2
	packagesExist(libavcodec) {
		DEFINES += HAS_AVCODEC
	}
}

PRECOMPILED_HEADER = precompiled.pch
precompile_header:!isEmpty(PRECOMPILED_HEADER) {
    DEFINES += USING_PCH
}

# Input
SOURCES += isoftplayer.cpp main.cpp
HEADERS += isoftplayer.h

IDE_BUILD_TREE = $$OUT_PWD/../../..
include(../qtcreator.pri)

win32 {
	DLLDESTDIR = $$IDE_APP_PATH
}

DESTDIR = $$IDE_LIBRARY_PATH

include(rpath.pri)

TARGET = $$qtLibraryTarget($$TARGET)

contains(QT_CONFIG, reduce_exports):CONFIG += hide_symbols

linux-* {
	target.path = /$$IDE_LIBRARY_BASENAME/qtcreator
	INSTALLS += target
}

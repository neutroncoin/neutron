TEMPLATE = app
TARGET = Neutron-qt
VERSION = 4.1.2
INCLUDEPATH += src src/json src/qt
DEFINES += QT_GUI BOOST_THREAD_USE_LIB BOOST_SPIRIT_THREADSAFE BOOST_NO_CXX11_SCOPED_ENUMS
CONFIG += no_include_pwd
CONFIG += thread
CONFIG += static
CONFIG += c++11

greaterThan(QT_MAJOR_VERSION, 4) {
    QT += widgets
    DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0
}

# for boost 1.37, add -mt to the boost libraries
# use: qmake BOOST_LIB_SUFFIX=-mt
# for boost thread win32 with _win32 sufix
# use: BOOST_THREAD_LIB_SUFFIX=_win32-...
# or when linking against a specific BerkelyDB version: BDB_LIB_SUFFIX=-4.8

# Dependency library locations can be customized with:
#    BOOST_INCLUDE_PATH, BOOST_LIB_PATH, BDB_INCLUDE_PATH,
#    BDB_LIB_PATH, OPENSSL_INCLUDE_PATH and OPENSSL_LIB_PATH respectively

OBJECTS_DIR = build
MOC_DIR = build
UI_DIR = build

# use: qmake "RELEASE=1"
contains(RELEASE, 1) {
    # Mac: compile for reasonable compatibility (10.10, 64-bit)
    macx:QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.10

    !windows:!macx {
        # Linux: static link
        LIBS += -Wl,-Bstatic
    }
}

LIBS += -pthread

!win32 {
# for extra security against potential buffer overflows: enable GCCs Stack Smashing Protection
QMAKE_CXXFLAGS *= -fstack-protector-all --param ssp-buffer-size=1
QMAKE_LFLAGS *= -fstack-protector-all --param ssp-buffer-size=1
# We need to exclude this for Windows cross compile with MinGW 4.2.x, as it will result in a non-working executable!
# This can be enabled for Windows, when we switch to MinGW >= 4.4.x.
}

# for extra security (see: https://wiki.debian.org/Hardening): this flag is GCC compiler-specific
QMAKE_CXXFLAGS *= -D_FORTIFY_SOURCE=2
# for extra security on Windows: enable ASLR and DEP via GCC linker flags
win32:QMAKE_LFLAGS *= -Wl,--dynamicbase -Wl,--nxcompat
# on Windows: enable GCC large address aware linker flag
win32 {
    !contains(QT_ARCH, x86_64) {
        message("Windows x86 (32bit) specific build")
        win32:QMAKE_LFLAGS *= -Wl,--large-address-aware
    } else {
        message("Windows x86 (64bit) specific build")
    }
}
win32:QMAKE_LFLAGS *= -static
# i686-w64-mingw32
win32:QMAKE_LFLAGS *= -static-libgcc -static-libstdc++

# use: qmake "USE_DBUS=1" or qmake "USE_DBUS=0"
linux:count(USE_DBUS, 0) {
    USE_DBUS=1
}
contains(USE_DBUS, 1) {
    message(Building with DBUS (Freedesktop notifications) support)
    DEFINES += USE_DBUS
    QT += dbus
}

contains(BITCOIN_NEED_QT_PLUGINS, 1) {
    DEFINES += BITCOIN_NEED_QT_PLUGINS
    QTPLUGIN += qcncodecs qjpcodecs qtwcodecs qkrcodecs qtaccessiblewidgets
}

# LevelDB library
INCLUDEPATH += src/leveldb/include src/leveldb/helpers
LIBS += $$PWD/src/leveldb/build/libleveldb.a
SOURCES += src/txdb-leveldb.cpp
message("Building LevelDB...")
!win32 {
   genleveldb.commands = cd $$PWD/src/leveldb && $$QMAKE_MKDIR build && cd build && cmake -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON -DCMAKE_AR=$$system("which $$first(QMAKE_AR)") -DCMAKE_LINKER=$$system("which $$QMAKE_LINK") -DCMAKE_RANLIB=$$system("which $$QMAKE_RANLIB") -DCMAKE_CXX_COMPILER=$$QMAKE_CXX -DLEVELDB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
} else {
    # make an educated guess about what the ranlib command is called
    isEmpty(QMAKE_RANLIB) {
        QMAKE_RANLIB = $$replace(QMAKE_STRIP, strip, ranlib)
    }
    LIBS += -lshlwapi
    genleveldb.commands = TARGET_OS=OS_WINDOWS_CROSSCOMPILE cd $$PWD/src/leveldb && $$QMAKE_MKDIR build && cd build && cmake -DCMAKE_CXX_COMPILER=$$QMAKE_CXX -DMAKE_CC_COMPILER=$$QMAKE_CC -DWIN32=True -DLEVELDB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
}
genleveldb.target = $$PWD/src/leveldb/build/libleveldb.a
genleveldb.depends = FORCE
PRE_TARGETDEPS += $$PWD/src/leveldb/build/libleveldb.a
QMAKE_EXTRA_TARGETS += genleveldb
# Gross ugly hack that depends on qmake internals, unfortunately there is no other way to do it.
QMAKE_CLEAN += $$PWD/src/leveldb/build/libleveldb.a ; cd $$PWD/src/leveldb/build ; $(MAKE) clean

# UniValue library
INCLUDEPATH += src/univalue/include
LIBS += -L$$PWD/src/univalue
#LIBS += -L$$PWD/src/univalue/libunivalue.la
HEADERS += src/univalue/include/univalue.h
SOURCES += src/univalue/lib/univalue.cpp src/univalue/lib/univalue_get.cpp src/univalue/lib/univalue_read.cpp src/univalue/lib/univalue_write.cpp
!win32 {
    !exists( $$PWD/src/univalue/libunivalue.la ) {
        message("Build libunivalue")
        # we use QMAKE_CXXFLAGS_RELEASE even without RELEASE=1 because we use RELEASE to indicate linking preferences not -O preferences
        gen_univalue_lib.commands = cd $$PWD/src/univalue && ./autogen.sh && ./configure && CC=$$QMAKE_CC CXX=$$QMAKE_CXX $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\" libunivalue.la && cd ..
    } else {
        message("Already built libunivalue")
    }
} else {
    # make an educated guess about what the ranlib command is called
    isEmpty(QMAKE_RANLIB) {
        QMAKE_RANLIB = $$replace(QMAKE_STRIP, strip, ranlib)
    }
    LIBS += -lshlwapi
    gen_univalue_lib.commands = cd $$PWD/src/univalue && ./autogen.sh && ./configure --host='i686-w64-mingw32.static' --build='x86_64-pc-linux-gnu' && CC=$$QMAKE_CC CXX=$$QMAKE_CXX TARGET_OS=OS_WINDOWS_CROSSCOMPILE $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\" libunivalue.la && cd ..
}
gen_univalue_lib.target = $$PWD/src/univalue/lunivalue.la
PRE_TARGETDEPS += $$PWD/src/univalue/lunivalue.la
QMAKE_EXTRA_TARGETS += gen_univalue_lib
QMAKE_CLEAN += cd $$PWD/src/univalue ; $(MAKE) clean

# regenerate src/build.h
!windows|contains(USE_BUILD_INFO, 1) {
    genbuild.depends = FORCE
    genbuild.commands = cd $$PWD; /bin/sh share/genbuild.sh $$OUT_PWD/build/build.h
    genbuild.target = $$OUT_PWD/build/build.h
    PRE_TARGETDEPS += $$OUT_PWD/build/build.h
    QMAKE_EXTRA_TARGETS += genbuild
    DEFINES += HAVE_BUILD_INFO
}

contains(USE_O3, 1) {
    message(Building O3 optimization flag)
    QMAKE_CXXFLAGS_RELEASE -= -O2
    QMAKE_CFLAGS_RELEASE -= -O2
    QMAKE_CXXFLAGS += -O3
    QMAKE_CFLAGS += -O3
}

*-g++-32 {
    message("32 platform, adding -msse2 flag")

    QMAKE_CXXFLAGS += -msse2
    QMAKE_CFLAGS += -msse2
}

QMAKE_CXXFLAGS_WARN_ON = -fdiagnostics-show-option -Wall -Wextra -Wno-unused-local-typedefs -Wno-ignored-qualifiers -Wformat -Wformat-security -Wno-unused-parameter -Wstack-protector
QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-variable -fpermissive -Wno-format-extra-args

windows:QMAKE_CXXFLAGS_WARN_ON += -Wno-cpp -Wno-maybe-uninitialized
!macx:QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-local-typedefs

# Input
DEPENDPATH += src src/json src/qt

HEADERS += src/activemasternode.h \
    src/addrdb.h \
    src/addrman.h \
    src/alert.h \
    src/allocators.h \
    src/backtrace.h \
    src/base58.h \
    src/bignum.h \
    src/bitcoinrpc.h \
    src/chainparams.h \
    src/checkpoints.h \
    src/clientversion.h \
    src/crypter.h \
    src/compat.h \
    src/coincontrol.h \
    src/darksend.h \
    src/db.h \
    src/init.h \
    src/ismine.h \
    src/kernel.h \
    src/key.h \
    src/keystore.h \
    src/main.h \
    src/masternode.h \
    src/miner.h \
    src/mruset.h \
    src/net.h \
    src/netaddress.h \
    src/netbase.h \
    src/noui.h \
    src/pbkdf2.h \
    src/protocol.h \
    src/random.h \
    src/robinhood.h \
    src/scheduler.h \
    src/script.h \
    src/scrypt.h \
    src/serialize.h \
    src/spork.h \
    src/streams.h \
    src/strlcpy.h \
    src/sync.h \
    src/threadinterrupt.h \
    src/threadsafety.h \
    src/timedata.h \
    src/txdb.h \
    src/txmempool.h \
    src/uint256.h \
    src/ui_interface.h \
    src/util.h \
    src/utilmoneystr.h \
    src/utilstrencodings.h \
    src/utiltime.h \
    src/validation.h \
    src/version.h \
    src/wallet.h \
    src/walletdb.h \
    src/json/json_spirit.h \
    src/json/json_spirit_error_position.h \
    src/json/json_spirit_reader_template.h \
    src/json/json_spirit_reader.h \
    src/json/json_spirit_stream_reader.h \
    src/json/json_spirit_value.h \
    src/json/json_spirit_utils.h \
    src/json/json_spirit_writer_template.h \
    src/json/json_spirit_writer.h \
    src/primitives/block.h \
    src/primitives/transaction.h \
    src/qt/aboutdialog.h \
    src/qt/addeditadrenalinenode.h \
    src/qt/addressbookpage.h \
    src/qt/addresstablemodel.h \
    src/qt/adrenalinenodeconfigdialog.h \
    src/qt/askpassphrasedialog.h \
    src/qt/bitcoinaddressvalidator.h \
    src/qt/bitcoinamountfield.h \
    src/qt/bitcoingui.h \
    src/qt/bitcoinunits.h \
    src/qt/clientmodel.h \
    src/qt/coincontroldialog.h \
    src/qt/coincontroltreewidget.h \
    src/qt/csvmodelwriter.h \
    src/qt/editaddressdialog.h \
    src/qt/guiutil.h \
    src/qt/guiconstants.h \
    src/qt/loggerpage.h \
    src/qt/masternodemanager.h \
    src/qt/networkstyle.h \
    src/qt/notificator.h \
    src/qt/optionsdialog.h \
    src/qt/optionsmodel.h \
    src/qt/overviewpage.h \
    src/qt/qvalidatedlineedit.h \
    src/qt/qvaluecombobox.h \
    src/qt/rpcconsole.h \
    src/qt/sendcoinsdialog.h \
    src/qt/sendcoinsentry.h \
    src/qt/signverifymessagedialog.h \
    src/qt/splashscreen.h \
    src/qt/stakereportdialog.h \
    src/qt/transactiondesc.h \
    src/qt/transactiondescdialog.h \
    src/qt/transactionfilterproxy.h \
    src/qt/transactionrecord.h \
    src/qt/transactiontablemodel.h \
    src/qt/transactionview.h \
    src/qt/walletmodel.h \
    src/rpc/register.h \
    src/script/standard.h

SOURCES += src/activemasternode.cpp \
    src/addrdb.cpp \
    src/addrman.cpp \
    src/alert.cpp \
    src/backtrace.cpp \
    src/bitcoinrpc.cpp \
    src/chainparams.cpp \
    src/checkpoints.cpp \
    src/clientversion.cpp \
    src/crypter.cpp \
    src/darksend.cpp \
    src/db.cpp \
    src/init.cpp \
    src/ismine.cpp \
    src/kernel.cpp \
    src/key.cpp \
    src/keystore.cpp \
    src/main.cpp \
    src/masternode.cpp \
    src/masternodeconfig.cpp \
    src/miner.cpp \
    src/net.cpp \
    src/netaddress.cpp \
    src/netbase.cpp \
    src/noui.cpp \
    src/protocol.cpp \
    src/pbkdf2.cpp \
    src/random.cpp \
    src/rpcblockchain.cpp \
    src/rpcdarksend.cpp \
    src/rpcdump.cpp \
    src/rpcmining.cpp \
    src/rpcnet.cpp \
    src/rpcrawtransaction.cpp \
    src/rpcwallet.cpp \
    src/scheduler.cpp \
    src/script.cpp \
    src/scrypt.cpp \
    src/scrypt-arm.S \
    src/scrypt-x86.S \
    src/scrypt-x86_64.S \
    src/spork.cpp \
    src/sync.cpp \
    src/threadinterrupt.cpp \
    src/timedata.cpp \
    src/txmempool.cpp \
    src/ui_interface.cpp \
    src/util.cpp \
    src/utilmoneystr.cpp \
    src/utilstrencodings.cpp \
    src/utiltime.cpp \
    src/validation.cpp \
    src/wallet.cpp \
    src/walletdb.cpp \
    src/primitives/block.cpp \
    src/primitives/transaction.cpp \
    src/qt/aboutdialog.cpp \
    src/qt/addeditadrenalinenode.cpp \
    src/qt/addressbookpage.cpp \
    src/qt/addresstablemodel.cpp \
    src/qt/adrenalinenodeconfigdialog.cpp \
    src/qt/askpassphrasedialog.cpp \
    src/qt/bitcoin.cpp \
    src/qt/bitcoinaddressvalidator.cpp \
    src/qt/bitcoinamountfield.cpp \
    src/qt/bitcoingui.cpp \
    src/qt/bitcoinstrings.cpp \
    src/qt/bitcoinunits.cpp \
    src/qt/clientmodel.cpp \
    src/qt/coincontroldialog.cpp \
    src/qt/coincontroltreewidget.cpp \
    src/qt/csvmodelwriter.cpp \
    src/qt/editaddressdialog.cpp \
    src/qt/guiutil.cpp \
    src/qt/loggerpage.cpp \
    src/qt/masternodemanager.cpp \
    src/qt/networkstyle.cpp \
    src/qt/notificator.cpp \
    src/qt/optionsdialog.cpp \
    src/qt/optionsmodel.cpp \
    src/qt/overviewpage.cpp \
    src/qt/qvalidatedlineedit.cpp \
    src/qt/qvaluecombobox.cpp \
    src/qt/rpcconsole.cpp \
    src/qt/sendcoinsdialog.cpp \
    src/qt/sendcoinsentry.cpp \
    src/qt/splashscreen.cpp \
    src/qt/stakereportdialog.cpp \
    src/qt/signverifymessagedialog.cpp \
    src/qt/transactiondesc.cpp \
    src/qt/transactiondescdialog.cpp \
    src/qt/transactionfilterproxy.cpp \
    src/qt/transactionrecord.cpp \
    src/qt/transactiontablemodel.cpp \
    src/qt/transactionview.cpp \
    src/qt/walletmodel.cpp \
    src/rpc/rpcmasternode.cpp \
    src/script/standard.cpp

RESOURCES += \
    src/qt/bitcoin.qrc

FORMS += \
    src/qt/forms/aboutdialog.ui \
    src/qt/forms/addeditadrenalinenode.ui \
    src/qt/forms/addressbookpage.ui \
    src/qt/forms/adrenalinenodeconfigdialog.ui \
    src/qt/forms/askpassphrasedialog.ui \
    src/qt/forms/coincontroldialog.ui \
    src/qt/forms/editaddressdialog.ui \
    src/qt/forms/loggerpage.ui \
    src/qt/forms/masternodemanager.ui \
    src/qt/forms/optionsdialog.ui \
    src/qt/forms/overviewpage.ui \
    src/qt/forms/rpcconsole.ui \
    src/qt/forms/sendcoinsdialog.ui \
    src/qt/forms/sendcoinsentry.ui \
    src/qt/forms/signverifymessagedialog.ui \
    src/qt/forms/stakereportdialog.ui \
    src/qt/forms/transactiondescdialog.ui

contains(USE_QRCODE, 1) {
HEADERS += src/qt/qrcodedialog.h
SOURCES += src/qt/qrcodedialog.cpp
FORMS += src/qt/forms/qrcodedialog.ui
}

CODECFORTR = UTF-8

# for lrelease/lupdate
# also add new translations to src/qt/bitcoin.qrc under translations/
TRANSLATIONS = $$files(src/qt/locale/bitcoin_*.ts)

isEmpty(QMAKE_LRELEASE) {
    win32:QMAKE_LRELEASE = $$[QT_INSTALL_BINS]\\lrelease.exe
    else:QMAKE_LRELEASE = $$[QT_INSTALL_BINS]/lrelease
}
isEmpty(QM_DIR):QM_DIR = $$PWD/src/qt/locale
# automatically build translations, so they can be included in resource file
TSQM.name = lrelease ${QMAKE_FILE_IN}
TSQM.input = TRANSLATIONS
TSQM.output = $$QM_DIR/${QMAKE_FILE_BASE}.qm
TSQM.commands = $$QMAKE_LRELEASE ${QMAKE_FILE_IN} -qm ${QMAKE_FILE_OUT}
TSQM.CONFIG = no_link
QMAKE_EXTRA_COMPILERS += TSQM

# "Other files" to show in Qt Creator
OTHER_FILES += \
    doc/*.rst doc/*.txt doc/README README.md res/bitcoin-qt.rc

# platform specific defaults, if not overridden on command line
isEmpty(BOOST_LIB_SUFFIX) {
    macx:BOOST_LIB_SUFFIX = -mt
    windows:BOOST_LIB_SUFFIX=-mgw49-mt-s-1_57
}

isEmpty(BOOST_THREAD_LIB_SUFFIX) {
    BOOST_THREAD_LIB_SUFFIX = $$BOOST_LIB_SUFFIX
    #win32:BOOST_THREAD_LIB_SUFFIX = _win32$$BOOST_LIB_SUFFIX
    #else:BOOST_THREAD_LIB_SUFFIX = $$BOOST_LIB_SUFFIX
}

isEmpty(BDB_LIB_PATH) {
    macx:BDB_LIB_PATH = /usr/local/Cellar/berkeley-db@4/4.8.30/lib
    windows:BDB_LIB_PATH=C:/dev/coindeps32/bdb-4.8/lib
}

isEmpty(BDB_LIB_SUFFIX) {
    macx:BDB_LIB_SUFFIX = -4.8
}

isEmpty(BDB_INCLUDE_PATH) {
    macx:BDB_INCLUDE_PATH = /usr/local/Cellar/berkeley-db@4/4.8.30/include
    windows:BDB_INCLUDE_PATH=C:/dev/coindeps32/bdb-4.8/include
}

isEmpty(BOOST_LIB_PATH) {
    macx:BOOST_LIB_PATH = /usr/local/Cellar/boost@1.60/1.60.0/lib
    windows:BOOST_LIB_PATH=C:/dev/coindeps32/boost_1_57_0/lib
}

isEmpty(BOOST_INCLUDE_PATH) {
    macx:BOOST_INCLUDE_PATH = /usr/local/Cellar/boost@1.60/1.60.0/include
    windows:BOOST_INCLUDE_PATH=C:/dev/coindeps32/boost_1_57_0/include
}

isEmpty(QRENCODE_LIB_PATH) {
    macx:QRENCODE_LIB_PATH = /usr/local/lib
}

isEmpty(QRENCODE_INCLUDE_PATH) {
    macx:QRENCODE_INCLUDE_PATH = /usr/local/include
}

isEmpty(MINIUPNPC_LIB_SUFFIX) {
    windows:MINIUPNPC_LIB_SUFFIX=-miniupnpc
}

isEmpty(MINIUPNPC_INCLUDE_PATH) {
    macx:MINIUPNPC_INCLUDE_PATH=/usr/local/Cellar/miniupnpc/2.1/include
    windows:MINIUPNPC_INCLUDE_PATH=C:/dev/coindeps32/miniupnpc-1.9
}

isEmpty(MINIUPNPC_LIB_PATH) {
    macx:MINIUPNPC_LIB_PATH=/usr/local/Cellar/miniupnpc/2.1/lib
    windows:MINIUPNPC_LIB_PATH=C:/dev/coindeps32/miniupnpc-1.9
}

isEmpty(OPENSSL_INCLUDE_PATH) {
    macx:OPENSSL_INCLUDE_PATH = /usr/local/Cellar/openssl/1.0.2s/include
    windows:OPENSSL_INCLUDE_PATH=C:/dev/coindeps32/openssl-1.0.1p/include
}

isEmpty(OPENSSL_LIB_PATH) {
    macx:OPENSSL_LIB_PATH = /usr/local//Cellar/openssl/1.0.2s/lib
    windows:OPENSSL_LIB_PATH=C:/dev/coindeps32/openssl-1.0.1p/lib
}

isEmpty(SECP256K1_LIB_PATH) {
    macx:SECP256K1_LIB_PATH = /usr/local/lib
    windows:SECP256K1_LIB_PATH=C:/dev/coindeps32/Secp256k1/lib
}

isEmpty(SECP256K1_INCLUDE_PATH) {
    macx:SECP256K1_INCLUDE_PATH = /usr/local/include
    windows:SECP256K1_INCLUDE_PATH=C:/dev/coindeps32/Secp256k1/include
}

# use: qmake "USE_UPNP=1" ( enabled by default; default)
#  or: qmake "USE_UPNP=0" (disabled by default)
#  or: qmake "USE_UPNP=-" (not supported)
# miniupnpc (http://miniupnp.free.fr/files/) must be installed for support
contains(USE_UPNP, -) {
    message(Building without UPNP support)
} else {
    message(Building with UPNP support)
    count(USE_UPNP, 0) {
        USE_UPNP=1
    }
    DEFINES += USE_UPNP=$$USE_UPNP MINIUPNP_STATICLIB STATICLIB
    INCLUDEPATH += $$MINIUPNPC_INCLUDE_PATH
    LIBS += $$join(MINIUPNPC_LIB_PATH,,-L,) -lminiupnpc
    win32:LIBS += -liphlpapi
}

# use: qmake "USE_QRCODE=1"
# libqrencode (http://fukuchi.org/works/qrencode/index.en.html) must be installed for support
contains(USE_QRCODE, 1) {
    message(Building with QRCode support)
    DEFINES += USE_QRCODE
    INCLUDEPATH += $$QRENCODE_INCLUDE_PATH
    LIBS += $$join(QRENCODE_LIB_PATH,,-L,) -lqrencode
}

# use: qmake "ENABLE_WALLET=1" (enabled by default; default)
#  or: qmake "ENABLE_WALLET=0" (disabled by default)
contains(ENABLE_WALLET, 0) {
    message(Building without Wallet enabled)
} else {
    message(Building with Wallet enabled)
    DEFINES += ENABLE_WALLET
}

windows:DEFINES += WIN32
windows:RC_FILE = src/qt/res/bitcoin-qt.rc
windows:RC_DEFINES = WINDRES_PREPROC

windows:!contains(MINGW_THREAD_BUGFIX, 0) {
    # At least qmake's win32-g++-cross profile is missing the -lmingwthrd
    # thread-safety flag. GCC has -mthreads to enable this, but it doesn't
    # work with static linking. -lmingwthrd must come BEFORE -lmingw, so
    # it is prepended to QMAKE_LIBS_QT_ENTRY.
    # It can be turned off with MINGW_THREAD_BUGFIX=0, just in case it causes
    # any problems on some untested qmake profile now or in the future.
    DEFINES += _MT BOOST_THREAD_PROVIDES_GENERIC_SHARED_MUTEX_ON_WIN
    QMAKE_LIBS_QT_ENTRY = -lmingwthrd $$QMAKE_LIBS_QT_ENTRY
}

macx:HEADERS += src/qt/macdockiconhandler.h src/qt/macnotificationhandler.h
macx:OBJECTIVE_SOURCES += src/qt/macdockiconhandler.mm src/qt/macnotificationhandler.mm
macx:LIBS += -framework Foundation -framework ApplicationServices -framework AppKit
macx:DEFINES += MAC_OSX MSG_NOSIGNAL=0
macx:ICON = src/qt/res/icons/neutron.icns
macx:QMAKE_TARGET_BUNDLE_PREFIX = "com.neutroncoin"

# Set libraries and includes at end, to use platform-defined defaults if not overridden
INCLUDEPATH += $$BOOST_INCLUDE_PATH $$BDB_INCLUDE_PATH $$OPENSSL_INCLUDE_PATH
LIBS += $$join(BOOST_LIB_PATH,,-L,) $$join(BDB_LIB_PATH,,-L,) $$join(OPENSSL_LIB_PATH,,-L,)
LIBS += -lssl -lcrypto -ldb_cxx$$BDB_LIB_SUFFIX
# -lgdi32 has to happen after -lcrypto (see  #681)
windows:LIBS += -lws2_32 -lshlwapi -lmswsock -lole32 -loleaut32 -luuid -lgdi32
LIBS += -lboost_system$$BOOST_LIB_SUFFIX -lboost_filesystem$$BOOST_LIB_SUFFIX -lboost_program_options$$BOOST_LIB_SUFFIX -lboost_thread$$BOOST_THREAD_LIB_SUFFIX -lboost_chrono$$BOOST_LIB_SUFFIX

contains(RELEASE, 1) {
    !windows:!macx {
        # Linux: turn dynamic linking back on for c/c++ runtime libraries
        LIBS += -Wl,-Bdynamic
    }
}

!windows:!macx {
    DEFINES += LINUX
    LIBS += -lrt -ldl
}

system($$QMAKE_LRELEASE -silent $$_PRO_FILE_)

# Compile for Ubuntu 20.04

##### NOTES: only command line daemon instructions currently
##### TODO: add GUI build instructions

## Install dependencies
```
sudo apt-get update
sudo apt-get install build-essential libminiupnpc-dev automake libtool cmake zlib1g-dev git
```

##### Manually compile Berkeley DB 4.8 from source instead of using libdb++-dev
```
	# get source
	mkdir -p ~/compile/berkeleydb
	cd ~/compile/berkeleydb
	wget http://download.oracle.com/berkeley-db/db-4.8.30.NC.tar.gz
	tar -xzvf db-4.8.30.NC.tar.gz

	# configure build
	cd db-4.8.30.NC/build_unix/
	# resolve build issue with Ubuntu 19.04 and above (ref: https://www.fsanmartin.co/compiling-berkeley-db-4-8-30-in-ubuntu-19/)
	sed -i 's/__atomic_compare_exchange/__atomic_compare_exchange_db/g' ../dbinc/atomic.h
	../dist/configure --enable-cxx --disable-shared --with-pic

	# compile and install
	make install

	# verify install
	ls -l /usr/local/BerkeleyDB.4.8
```

##### Manually compile latest libssl1.0.2
```
	mkdir -p ~/compile/openssl1.0
	cd ~/compile/openssl1.0
	wget https://www.openssl.org/source/openssl-1.0.2u.tar.gz  
	gunzip openssl-1.0.2u.tar.gz 
	tar xf openssl-1.0.2u.tar 
	cd openssl-1.0.2u/
	./config --prefix=/usr/local/openssl/openssl-1.0.2u --openssldir=/usr/local/openssl/openssl-1.0.2u
	make install
	ls /usr/local/openssl/openssl-1.0.2u/
```

##### Manually compile boost 1.62 (ref: https://anycoder.wordpress.com/2014/04/28/building-boost/, https://www.boost.org/doc/libs/1_62_0/more/getting_started/unix-variants.html)
```
	mkdir -p ~/compile/boost
	cd ~/compile/boost
	wget https://sourceforge.net/projects/boost/files/boost/1.62.0/boost_1_62_0.tar.bz2
	tar xvfo boost_1_62_0.tar.bz2
	cd boost_1_62_0
 
	# Show available libraries
	./bootstrap.sh --show-libraries

	# only include certain extra libraries
	./bootstrap.sh --with-libraries=atomic,chrono,filesystem,program_options,thread --prefix=/usr/local/boost/boost-1.62.0

	# build
	./b2 install
	###./b2 toolset=gcc cxxflags=-std=gnu++0x
```

## Compile wallet

##### Get neutron source
```
	mkdir -p ~/compile/neutron
	cd ~/compile/neutron
	NEUTRON_RELEASE_TAG=`curl --silent "https://api.github.com/repos/neutroncoin/neutron/releases/latest" | grep -Po '"tag_name": "\K.*?(?=")'`
	git clone -b master --recursive https://github.com/neutroncoin/neutron.git neutron_$NEUTRON_RELEASE_TAG
	cd neutron_$NEUTRON_RELEASE_TAG
```

##### [Optional] Allocate more memory for compile
```
sudo dd if=/dev/zero of=/var/swap.img bs=1024k count=1000
sudo mkswap /var/swap.img
sudo chmod 600 /var/swap.img
sudo swapon /var/swap.img
```

## Compile neutron daemon without GUI (command line only)
```
	cd src
	BDB_INCLUDE_PATH=/usr/local/BerkeleyDB.4.8/include \
	BDB_LIB_PATH=/usr/local/BerkeleyDB.4.8/lib \
	OPENSSL_INCLUDE_PATH=/usr/local/openssl/openssl-1.0.2u/include \
	OPENSSL_LIB_PATH=/usr/local/openssl/openssl-1.0.2u/lib \
	BOOST_INCLUDE_PATH=/usr/local/boost/boost-1.62.0/include \
	BOOST_LIB_PATH=/usr/local/boost/boost-1.62.0/lib \
	STATIC=1 \
	make -f makefile.unix
```

##### Copy compiled daemon to bin
```
	cp neutrond /usr/bin/neutrond
```

##### Execute Neutron daemon from anywhere, check status 
```
	neutrond -daemon
	neutrond getdebuginfo
```

##### Compile with GUI
```
    TBD
```


----------------------------------------------------------------------------------------


# Compile for Ubuntu 18.04

## Install dependencies
```
sudo apt-get update
sudo apt-get install build-essential libboost1.62-all-dev libssl1.0-dev libminiupnpc-dev automake libtool cmake

# [optional] only needed for GUI
sudo apt-get install qt5-default qttools5-dev-tools
```

##### Manually compile Berkeley DB 4.8 from source instead of using libdb++-dev

```
# get source
mkdir -p ~/compile/berkeleydb
cd ~/compile/berkeleydb
wget http://download.oracle.com/berkeley-db/db-4.8.30.NC.tar.gz
tar -xzvf db-4.8.30.NC.tar.gz

# configure build
cd db-4.8.30.NC/build_unix/
../dist/configure --enable-cxx --disable-shared --with-pic

# compile and install
make install
```

##### [Optional, only for GUI] Manually compile qrencode 3.4.4 from source instead of using libqrencode-dev

```
mkdir -p ~/compile/qrencode
cd ~/compile/qrencode
wget https://fukuchi.org/works/qrencode/qrencode-3.4.4.tar.gz
tar zxvf qrencode-3.4.4.tar.gz
cd qrencode-3.4.4

# install dependencies
sudo apt-get install libpng-dev

# configure build
./configure --enable-static
# compile and install
make install
```

## Compile wallet

##### Get neutron source
```
mkdir -p ~/compile/neutron
cd ~/compile/neutron
NEUTRON_RELEASE_TAG=`curl --silent "https://api.github.com/repos/neutroncoin/neutron/releases/latest" | grep -Po '"tag_name": "\K.*?(?=")'`
git clone -b master --recursive https://github.com/neutroncoin/neutron.git neutron_$NEUTRON_RELEASE_TAG
cd neutron_$NEUTRON_RELEASE_TAG
```

##### [Optional] Allocate more memory for compile
```
sudo dd if=/dev/zero of=/var/swap.img bs=1024k count=1000
sudo mkswap /var/swap.img
sudo chmod 600 /var/swap.img
sudo swapon /var/swap.img
```

## Compile without GUI (command line only)
```
cd src
CXXFLAGS="-I/usr/local/BerkeleyDB.4.8/include" \
LDFLAGS="-L/usr/local/BerkeleyDB.4.8/lib" \
STATIC=1 \
make -f makefile.unix
```

## Compile with GUI
```
BDB_INC_PATH=/usr/local/BerkeleyDB.4.8/include
BDB_LIB_PATH=/usr/local/BerkeleyDB.4.8/lib
qmake \
    BDB_INCLUDE_PATH=$BDB_INC_PATH \
    BDB_LIB_PATH=$BDB_LIB_PATH \
    \
    USE_QRCODE=1 \
    USE_UPNP=1 \
    RELEASE=1 \
    USE_BUILD_INFO=1 \
    neutron-qt.pro
make
```

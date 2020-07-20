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

##### Get source
```
mkdir -p ~/compile/neutron
cd ~/compile/neutron
RELEASE_TAG=`curl --silent "https://api.github.com/repos/neutroncoin/neutron/releases/latest" | grep -Po '"tag_name": "\K.*?(?=")'`
wget https://github.com/neutroncoin/neutron/archive/$RELEASE_TAG.tar.gz
tar -xvf *.tar.gz --strip-components=1
```

##### [Optional] Allocate more memory for compile
```
sudo dd if=/dev/zero of=/var/swap.img bs=1024k count=1000
sudo mkswap /var/swap.img
sudo chmod 600 /var/swap.img
sudo swapon /var/swap.img
```

##### Compile without GUI (command line only)
```
cd src
CXXFLAGS="-I/usr/local/BerkeleyDB.4.8/include" \
LDFLAGS="-L/usr/local/BerkeleyDB.4.8/lib" \
STATIC=1 \
make -f makefile.unix
```

##### Compile with GUI
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

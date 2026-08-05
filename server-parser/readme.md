# setup

```
    sudo apt install clang
```

# install gpiod v2

```
    sudo apt install autoconf-archive libtool pkg-config autoconf automake build-essential

    git clone https://github.com/brgl/libgpiod.git

    cd libgpiod
    
    git checkout v2.3.1 

    sudo apt install -y meson ninja-build

    cd ~/libgpiod-2.3.1/
    meson setup build --prefix=/usr/local -Dtools=enabled
    ninja -C build
    sudo ninja -C build install
    sudo ldconfig
```

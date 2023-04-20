#!/bin/bash -x
# set -e，这个参数的含义是，当命令发生错误的时候，停止脚本的执行。
# set -ex


# 替换子模块的地址
grep -q ghproxy .gitmodules
if [ $? -ne 0 ]; then
    sed -i 's/https:\/\/github/https:\/\/ghproxy.com\/https:\/\/github/g' .gitmodules
fi

if [ -d .git ]; then
    # git submodule update --init --recursive
    git submodule update --init
fi

#
for file in `find ./src -name .gitmodules`
do
    if [ ! -f "$file" ]; then
        continue
    fi
    
    grep -q ghproxy $file
    if [ $? -ne 0 ]; then    
        sed -i 's/https:\/\/github/https:\/\/ghproxy.com\/https:\/\/github/g' $file
    fi
done


# 修改pip的源
if [ ! -d ~/.pip ]; then
    mkdir ~/.pip
fi

cat << EOF > ~/.pip/pip.conf
[global]
index-url = https://pypi.tuna.tsinghua.edu.cn/simple/
[install]
trusted-host=pypi.tuna.tsinghua.edu.cn
EOF


# 安装依赖
./install-deps.sh

# 编译ceph
./do_cmake.sh -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBEXECDIR=/usr/lib
cd build
make -j 4

make install
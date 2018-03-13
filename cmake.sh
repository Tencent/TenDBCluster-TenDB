#!/bin/sh 

if [ $# -ne 2 ]
then
        echo "Error input! USAGE: sh $0 tmysql_version release_or_debug. like $0 tmysql-3.1 release"
        exit 1;
fi

gccdir=/usr/local/gcc-4.7.3/

suffix="-$1"
debug_flag=" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_CONFIG=mysql_release "
bld_dir="bld"

debug_arg=`echo $2 | tr 'a-z' 'A-Z'`

if [ "$debug_arg"x = "DEBUG"x ]
then
	suffix="$suffix-debug"
	debug_flag=" -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=ON "
	bld_dir="bld_debug"	
fi

echo "$suffix $debug_flag $bld_dir"

mkdir -p $bld_dir
cd $bld_dir

rm -f CMakeCache.txt
#export LD_LIBRARY_PATH=$gccdir/lib64/:$LD_LIBRARY_PATH

cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=/home/mysql/boost/ -DWITHOUT_TOKUDB_STORAGE_ENGINE=1 -DMYSQL_SERVER_SUFFIX=$suffix $debug_flag -DFEATURE_SET=community  -DWITH_EMBEDDED_SERVER=OFF -DCMAKE_C_COMPILER=$gccdir/bin/gcc -DCMAKE_CXX_COMPILER=$gccdir/bin/g++ -DCMAKE_INSTALL_PREFIX=/usr/local/mysql -DCMAKE_CXX_FLAGS="-static-libgcc -static-libstdc++" -DCMAKE_C_FLAGS="-static-libgcc" -DWITH_QUERY_RESPONSE_TIME=on
#cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=/home/mysql/boost/ -DWITHOUT_TOKUDB_STORAGE_ENGINE=1 -DMYSQL_SERVER_SUFFIX=$suffix $debug_flag -DFEATURE_SET=community  -DWITH_EMBEDDED_SERVER=OFF -DCMAKE_INSTALL_PREFIX=/usr/local/mysql -DWITH_QUERY_RESPONSE_TIME=on

make -j
#make package 

cd ..

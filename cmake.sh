#!/bin/sh 

usage(){
    echo -e "Usage: $0 [OPTIONS]"
    echo -e "compile MySQL Server source code"
    echo -e ""
    echo -e " -H --help		display help info ."
    echo -e " -t --tar		make package to tar.gz file"
    echo -e " -d --directory		directory to install mysql"
    echo -e " -v --version		mysql server version"
    echo -e " -b --boost-dir		boost library directory"
    echo -e " -i --install		do install"
    echo -e " --test			do mysql-test-run"
    echo -e " --debug		compile with debug info"
    echo -e "--------------------------------------------------------"
    echo -e "  version		default 3.1"
    echo -e "  debug			default to false"
    echo -e "  directory		default to /usr/local/mysql"
    echo -e "  boost			default to /home/mysql/boost"
    echo -e ""

    exit 1
}

do_install=0
debug=0
do_tar=0
version=3.1
do_test=0
debug_flag=" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_CONFIG=mysql_release "
bld_dir="bld"
static_flag=" -DCMAKE_CXX_FLAGS=-static-libstdc++ -DCMAKE_C_FLAGS=-static-libgcc " 
boost_dir=/home/mysql/boost/
install_dir=/usr/local/mysql
gccdir=/usr/local/gcc-4.7.3
export LD_LIBRARY_PATH=$gccdir/lib64/:$LD_LIBRARY_PATH

TEMP=`getopt -o b:d:hitv: --long debug,test,help,install,tar,version:,directory:,boost-dir:,verion: \
	-n "Try $0 --help for more information" -- "$@"`

if [ $? != 0 ]
then
    echo "script abnormal exit"
    exit 1
fi
if [ $# -eq 0 ]
then
    usage
fi

eval set -- "$TEMP"

while true
do
    case $1 in
	-d|--directory)	install_dir=$2; shift 2;;
	-h|--help)	usage; shift;;
	-i|--install)	do_install=1; shift;;
	-t|--tar)	do_tar=1; shift;;
	-v|--version)	version=$2; shift 2;;
	-b|--boost-dir)	boost_dir=$2; shift 2;;
	--debug)	debug=1; shift;;
	--test)		do_test=1; shift;;
	--) shift ; break;;
	*) usage;
	esac
done

suffix="-tmysql-$version"

if [ $debug == 1 ]
then
    suffix="$suffix-debug"
    debug_flag=" -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=ON "
    bld_dir="bld_debug"
    #static_flag=""
fi


mkdir -p $bld_dir
cd $bld_dir

rm -f CMakeCache.txt

cmd="cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=$boost_dir -DWITH_ZLIB=bundled -DWITHOUT_TOKUDB_STORAGE_ENGINE=1 -DMYSQL_SERVER_SUFFIX=$suffix $debug_flag -DFEATURE_SET=community  -DWITH_EMBEDDED_SERVER=OFF -DCMAKE_C_COMPILER=$gccdir/bin/gcc -DCMAKE_CXX_COMPILER=$gccdir/bin/g++ -DCMAKE_INSTALL_PREFIX=$install_dir -DWITH_QUERY_RESPONSE_TIME=on $static_flag"
echo "compile args:"
echo "$cmd"
$cmd

make VERBOSE=1 -j `grep -c '^processor' /proc/cpuinfo`

if [ $do_install == 1 ]
then
    make install
fi

if [ $do_tar == 1 ]
then
    make package
fi

if [ $do_test == 1 ]
then
    cd mysql-test
    perl mysql-test-run.pl --max-test-fail=0 --force --parallel=8 &>test.log
    cd ..
fi

cd ..

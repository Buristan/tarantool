#!/bin/bash
set -e

rm_file='rm -f'
rm_dir='rm -rf'
mk_dir='mkdir -p'

alloss='ubuntu debian centos el fedora'

get_os_dists()
{
    os=$1
    alldists=

    if [ "$os" == "ubuntu" ]; then
        alldists='trusty xenial cosmic disco bionic eoan'
    elif [ "$os" == "debian" ]; then
        alldists='jessie stretch buster'
    elif [ "$os" == "centos" -o "$os" == "el" ]; then
        alldists='6 7 8'
    elif [ "$os" == "fedora" ]; then
        alldists='27 28 29 30 31'
    fi

    echo "$alldists"
}

ws_prefix=/tmp/tarantool_repo_s3
create_lockfile()
{
    lockfile -l 1000 $1
}

usage()
{
    cat <<EOF
Usage: $0 -b <S3 bucket> -o <OS name> -d <OS distribuition> [-p <product>] <path>
Options:
    -b|--bucket
        MCS S3 bucket which will be used for storing the packages.
        Name of the package one of the appropriate Tarantool branch:
            master: 2x
            2.3: 2_3
            2.2: 2_2
            1.10: 1_10
    -o|--os
        OS to be checked, one of the list (NOTE: centos == el):
            $alloss
    -d|--distribution
        Distribution appropriate to the given OS:
EOF
    for os in $alloss ; do
        echo "            $os: <"`get_os_dists $os`">"
    done
    cat <<EOF
    -p|--product
         Product name to be packed with, default name is 'tarantool'
    -h|--help
         Usage help message
    <path>
         Path points to the directory with deb/prm packages to be used.
         Script can be used in one of 2 modes:
          - path with binaries packages for a single distribution
	  - path with 'pool' directory with APT repository (only: debian|ubuntu)
EOF
}

for i in "$@"
do
case $i in
    -b=*|--bucket=*)
    branch="${i#*=}"
    if [ "$branch" != "2x" -a "$branch" != "2_3" -a "$branch" != "2_2" -a "$branch" != "1_10" ]; then
        echo "ERROR: bucket '$branch' is not supported"
        usage
        exit 1
    fi
    shift # past argument=value
    ;;
    -o=*|--os=*)
    os="${i#*=}"
    if [ "$os" == "el" ]; then
        os=centos
    fi
    if ! echo $alloss | grep -F -q -w $os ; then
        echo "ERROR: OS '$os' is not supported"
        usage
        exit 1
    fi
    shift # past argument=value
    ;;
    -d=*|--distribution=*)
    DIST="${i#*=}"
    shift # past argument=value
    ;;
    -p=*|--product=*)
    product="${i#*=}"
    shift # past argument=value
    ;;
    -h|--help)
    usage
    exit 0
    ;;
    *)
    repo="${i#*=}"
    pushd $repo >/dev/null ; repo=$PWD ; popd >/dev/null
    shift # past argument=value
    ;;
esac
done

# check that all needed options were set
if [ "$branch" == "" ]; then
    echo "ERROR: need to set -b|--bucket bucket option, check usage"
    usage
    exit 1
fi
if [ "$os" == "" ]; then
    echo "ERROR: need to set -o|--os OS name option, check usage"
    usage
    exit 1
fi
alldists=`get_os_dists $os`
if [ -n "$DIST" ] && ! echo $alldists | grep -F -q -w $DIST ; then
    echo "ERROR: set distribution at options '$DIST' not found at supported list '$alldists'"
    usage
    exit 1
fi

# set the path with binaries
product=$product
if [ "$product" == "" ]; then
    product=tarantool
fi
proddir=`echo $product | head -c 1`

# set the path with binaries
if [ "$repo" == "" ]; then
    repo=.
fi

aws='aws --endpoint-url https://hb.bizmrg.com'
s3="s3://tarantool_repo/$branch/$os"

# The 'pack_deb' function especialy created for DEB packages. It works
# with DEB packing OS like Ubuntu, Debian. It is based on globaly known
# tool 'reprepro' from:
#     https://wiki.debian.org/DebianRepository/SetupWithReprepro
# This tool works with complete number of distributions of the given OS.
# Result of the routine is the debian package for APT repository with
# file structure equal to the Debian/Ubuntu:
#     http://ftp.am.debian.org/debian/pool/main/t/tarantool/
#     http://ftp.am.debian.org/ubuntu/pool/main/t/
function pack_deb {
    # we need to push packages into 'main' repository only
    component=main

    # debian has special directory 'pool' for packages
    debdir=pool

    # get packages from pointed location either mirror path
    if [ "$repo" == "" ] ; then
        repo=/var/spool/apt-mirror/mirror/packagecloud.io/tarantool/$branch/$os
    fi
    if [ ! -d $repo/$debdir ] && ( [ "$DIST" == "" ] || ! ls $repo/*.deb $repo/*.dsc $repo/*.tar.*z >/dev/null 2>&1 ) ; then
        echo "ERROR: Current '$repo' path doesn't have any of the following:"
        echo "Usage with set distribuition with option '-d' and packages: $0 [path with *.deb *.dsc *.tar.*z files]"
        echo "Usage with repositories: $0 [path to repository with '$debdir' subdirectory]"
        exit 1
    fi

    # temporary lock the publication to the repository
    ws=${ws_prefix}_${branch}_${os}
    ws_lockfile=${ws}.lock
    create_lockfile $ws_lockfile

    # create temporary workspace with repository copy
    $rm_dir $ws
    $mk_dir $ws

    # script works in one of 2 modes:
    # - path with binaries packages for a single distribution
    # - path with 'pool' directory with APT repository
    if [ "$DIST" != "" ] && ls $repo/*.deb $repo/*.dsc $repo/*.tar.*z >/dev/null 2>&1 ; then
        # copy single distribution with binaries packages
        repopath=$ws/pool/${DIST}/main/$proddir/$product
        $mk_dir ${repopath}
        cp $repo/*.deb $repo/*.dsc $repo/*.tar.*z $repopath/.
    elif [ -d $repo/$debdir ]; then
        # copy 'pool' directory with APT repository
        cp -rf $repo/$debdir $ws/.
    else
        echo "ERROR: neither distribution option '-d' with files $repo/*.deb $repo/*.dsc $repo/*.tar.*z set nor '$repo/$debdir' path found"
	usage
	$rm_file $wslock
	exit 1
    fi
    cd $ws

    # create the configuration file for 'reprepro' tool
    confpath=$ws/conf
    $rm_dir $confpath
    $mk_dir $confpath

    for dist in $alldists ; do
        cat <<EOF >>$confpath/distributions
Origin: Tarantool
Label: tarantool.org
Suite: stable
Codename: $dist
Architectures: amd64 source
Components: main
Description: Unofficial Ubuntu Packages maintained by Tarantool
SignWith: 91B625E5
DebIndices: Packages Release . .gz .bz2
UDebIndices: Packages . .gz .bz2
DscIndices: Sources Release .gz .bz2

EOF
done

    # create standalone repository with separate components
    for dist in $alldists ; do
        echo =================== DISTRIBUTION: $dist =========================
        updated_deb=0
        updated_dsc=0

        # 1(binaries). use reprepro tool to generate Packages file
        for deb in $ws/$debdir/$dist/$component/*/*/*.deb ; do
            [ -f $deb ] || continue
            locdeb=`echo $deb | sed "s#^$ws\/##g"`
            echo "DEB: $deb"
            # register DEB file to Packages file
            reprepro -Vb . includedeb $dist $deb
            # reprepro copied DEB file to local component which is not needed
            $rm_dir $debdir/$component
            # to have all packages avoid reprepro set DEB file to its own registry
            $rm_dir db
            # copy Packages file to avoid of removing by the new DEB version
            for packages in dists/$dist/$component/binary-*/Packages ; do
                if [ ! -f $packages.saved ] ; then
                    # get the latest Packages file from S3
                    $aws s3 ls "$s3/$packages" 2>/dev/null && \
                        $aws s3 cp --acl public-read \
                        "$s3/$packages" $packages.saved || \
                        touch $packages.saved
                fi
                # check if the DEB file already exists in Packages from S3
                if grep "^`grep "^SHA256: " $packages`$" $packages.saved ; then
                    echo "WARNING: DEB file already registered in S3!"
                    continue
                fi
                # store the new DEB entry
                cat $packages >>$packages.saved
                # save the registered DEB file to S3
                $aws s3 cp --acl public-read $deb $s3/$locdeb
                updated_deb=1
            done
        done

        # 1(sources). use reprepro tool to generate Sources file
        for dsc in $ws/$debdir/$dist/$component/*/*/*.dsc ; do
            [ -f $dsc ] || continue
            locdsc=`echo $dsc | sed "s#^$ws\/##g"`
            echo "DSC: $dsc"
            # register DSC file to Sources file
            reprepro -Vb . includedsc $dist $dsc
            # reprepro copied DSC file to component which is not needed
            $rm_dir $debdir/$component
            # to have all sources avoid reprepro set DSC file to its own registry
            $rm_dir db
            # copy Sources file to avoid of removing by the new DSC version
            sources=dists/$dist/$component/source/Sources
            if [ ! -f $sources.saved ] ; then
                # get the latest Sources file from S3
                $aws s3 ls "$s3/$sources" && \
                    $aws s3 cp --acl public-read "$s3/$sources" $sources.saved || \
                    touch $sources.saved
            fi
            # WORKAROUND: unknown why, but reprepro doesn`t save the Sources file
            gunzip -c $sources.gz >$sources
            # check if the DSC file already exists in Sources from S3
            hash=`grep '^Checksums-Sha256:' -A3 $sources | \
                tail -n 1 | awk '{print $1}'`
            if grep " $hash .*\.dsc$" $sources.saved ; then
                echo "WARNING: DSC file already registered in S3!"
                continue
            fi
            # store the new DSC entry
            cat $sources >>$sources.saved
            # save the registered DSC file to S3
            $aws s3 cp --acl public-read $dsc $s3/$locdsc
            tarxz=`echo $locdsc | sed 's#\.dsc$#.debian.tar.xz#g'`
            $aws s3 cp --acl public-read $ws/$tarxz "$s3/$tarxz"
            orig=`echo $locdsc | sed 's#-1\.dsc$#.orig.tar.xz#g'`
            $aws s3 cp --acl public-read $ws/$orig "$s3/$orig"
            updated_dsc=1
        done

        # check if any DEB/DSC files were newly registered
        [ "$updated_deb" == "0" -a "$updated_dsc" == "0" ] && \
            continue || echo "Updating dists"

        # finalize the Packages file
        for packages in dists/$dist/$component/binary-*/Packages ; do
            mv $packages.saved $packages
        done

        # 2(binaries). update Packages file archives
        for packpath in dists/$dist/$component/binary-* ; do
            pushd $packpath
            sed "s#Filename: $debdir/$component/#Filename: $debdir/$dist/$component/#g" -i Packages
            bzip2 -c Packages >Packages.bz2
            gzip -c Packages >Packages.gz
            popd
        done

        # 2(sources). update Sources file archives
        pushd dists/$dist/$component/source
        sed "s#Directory: $debdir/$component/#Directory: $debdir/$dist/$component/#g" -i Sources
        bzip2 -c Sources >Sources.bz2
        gzip -c Sources >Sources.gz
        popd

        # 3. update checksums entries of the Packages* files in *Release files
	# NOTE: it is stable structure of the *Release files when the checksum
	#       entries in it in the following way:
	# MD5Sum:
        #  <checksum> <size> <file orig>
        #  <checksum> <size> <file debian>
        # SHA1:
        #  <checksum> <size> <file orig>
        #  <checksum> <size> <file debian>
        # SHA256:
        #  <checksum> <size> <file orig>
        #  <checksum> <size> <file debian>
	#       The script bellow puts 'md5' value at the 1st found file entry,
	#       'sha1' - at the 2nd and 'sha256' at the 3rd
        pushd dists/$dist
        for file in `grep " $component/" Release | awk '{print $3}' | sort -u` ; do
            sz=`stat -c "%s" $file`
            md5=`md5sum $file | awk '{print $1}'`
            sha1=`sha1sum $file | awk '{print $1}'`
            sha256=`sha256sum $file | awk '{print $1}'`
            awk 'BEGIN{c = 0} ; {
                if ($3 == p) {
                    c = c + 1
                    if (c == 1) {print " " md  " " s " " p}
                    if (c == 2) {print " " sh1 " " s " " p}
                    if (c == 3) {print " " sh2 " " s " " p}
                } else {print $0}
            }' p="$file" s="$sz" md="$md5" sh1="$sha1" sh2="$sha256" \
            Release >Release.new
            mv Release.new Release
        done
        # resign the selfsigned InRelease file
        $rm_file InRelease
        gpg --clearsign -o InRelease Release
        # resign the Release file
        $rm_file Release.gpg
        gpg -abs -o Release.gpg Release
        popd

        # 4. sync the latest distribution path changes to S3
        $aws s3 sync --acl public-read dists/$dist "$s3/dists/$dist"
    done

    # unlock the publishing
    $rm_file $ws_lockfile
}

# The 'pack_rpm' function especialy created for RPM packages. It works
# with RPM packing OS like Centos, Fedora. It is based on globaly known
# tool 'createrepo' from:
#     https://linux.die.net/man/8/createrepo
# This tool works with single distribution of the given OS.
# Result of the routine is the rpm package for YUM repository with
# file structure equal to the Centos/Fedora:
#     http://mirror.centos.org/centos/7/os/x86_64/Packages/
#     http://mirrors.kernel.org/fedora/releases/30/Everything/x86_64/os/Packages/t/
function pack_rpm {
    if ! ls $repo/*.rpm >/dev/null 2>&1 ; then
        echo "ERROR: Current '$repo' has:"
        ls -al $repo
        echo "Usage: $0 [path with *.rpm files]"
        exit 1
    fi

    # temporary lock the publication to the repository
    ws=${prefix_lockfile}_${branch}_${os}_${DIST}
    ws_lockfile=${ws}.lock
    create_lockfile $ws_lockfile

    # create temporary workspace with packages copies
    $rm_dir $ws
    $mk_dir $ws
    cp $repo/*.rpm $ws/.
    cd $ws

    # set the paths
    if [ "$os" == "centos" ]; then
        repopath=$DIST/os/x86_64
        rpmpath=Packages
    elif [ "$os" == "fedora" ]; then
        repopath=releases/$DIST/Everything/x86_64/os
        rpmpath=Packages/$proddir
    fi
    packpath=$repopath/$rpmpath

    # prepare local repository with packages
    $mk_dir $packpath
    mv *.rpm $packpath/.
    cd $repopath

    # copy the current metadata files from S3
    mkdir repodata.base
    for file in `$aws s3 ls $s3/$repopath/repodata/ | awk '{print $NF}'` ; do
        $aws s3 ls $s3/$repopath/repodata/$file || continue
        $aws s3 cp $s3/$repopath/repodata/$file repodata.base/$file
    done

    # create the new repository metadata files
    createrepo --no-database --update --workers=2 --compress-type=gz --simple-md-filenames .
    mv repodata repodata.adding

    # merge metadata files
    mkdir repodata
    head -n 2 repodata.adding/repomd.xml >repodata/repomd.xml
    for file in filelists.xml other.xml primary.xml ; do
        # 1. take the 1st line only - to skip the line with number of packages which is not needed
        zcat repodata.adding/$file.gz | head -n 1 >repodata/$file
        # 2. take 2nd line with metadata tag and update the packages number in it
        packsold=0
        if [ -f repodata.base/$file.gz ] ; then
        packsold=`zcat repodata.base/$file.gz | head -n 2 | tail -n 1 | sed 's#.*packages="\(.*\)".*#\1#g'`
        fi
        packsnew=`zcat repodata.adding/$file.gz | head -n 2 | tail -n 1 | sed 's#.*packages="\(.*\)".*#\1#g'`
        packs=$(($packsold+$packsnew))
        zcat repodata.adding/$file.gz | head -n 2 | tail -n 1 | sed "s#packages=\".*\"#packages=\"$packs\"#g" >>repodata/$file
        # 3. take only 'package' tags from new file
        zcat repodata.adding/$file.gz | tail -n +3 | head -n -1 >>repodata/$file
        # 4. take only 'package' tags from old file if exists
        if [ -f repodata.base/$file.gz ] ; then
            zcat repodata.base/$file.gz | tail -n +3 | head -n -1 >>repodata/$file
        fi
        # 5. take the last closing line with metadata tag
        zcat repodata.adding/$file.gz | tail -n 1 >>repodata/$file

        # get the new data
        chsnew=`sha256sum repodata/$file | awk '{print $1}'`
        sz=`stat --printf="%s" repodata/$file`
        gzip repodata/$file
        chsgznew=`sha256sum repodata/$file.gz | awk '{print $1}'`
        szgz=`stat --printf="%s" repodata/$file.gz`
        timestamp=`date +%s -r repodata/$file.gz`

        # add info to repomd.xml file
        name=`echo $file | sed 's#\.xml$##g'`
	cat <<EOF >>repodata/repomd.xml
<data type="$name">
  <checksum type="sha256">$chsgznew</checksum>
  <open-checksum type="sha256">$chsnew</open-checksum>
  <location href="repodata/$file.gz"/>
  <timestamp>$timestamp</timestamp>
  <size>$szgz</size>
  <open-size>$sz</open-size>
</data>"
EOF
    done
    tail -n 1 repodata.adding/repomd.xml >>repodata/repomd.xml
    gpg --detach-sign --armor repodata/repomd.xml

    # copy the packages to S3
    for file in $rpmpath/*.rpm ; do
        $aws s3 cp --acl public-read $file "$s3/$repopath/$file"
    done

    # update the metadata at the S3
    $aws s3 sync --acl public-read repodata "$s3/$repopath/repodata"

    # unlock the publishing
    $rm_file $ws_lockfile
}

if [ "$os" == "ubuntu" -o "$os" == "debian" ]; then
    pack_deb
elif [ "$os" == "centos" -o "$os" == "fedora" ]; then
    pack_rpm
else
    echo "USAGE: given OS '$os' is not supported, use any single from the list: $alloss"
    usage
    exit 1
fi

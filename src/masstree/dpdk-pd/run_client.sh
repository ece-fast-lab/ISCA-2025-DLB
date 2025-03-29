#!/usr/bin/env bash
#make
#i=1
dirName=$(echo $1)
serverQpn=$(echo $2)
servTimeDist=$(echo $3)
serverIPAddr=$(echo $4)
ib_devname=$(echo $5)
gid=$(echo $6)
ratios=$(echo $7)
percents=$(echo $8)

#rps=0
#rps_prev=0
#break_flag=0
: '
if [[ "$servTimeDist" == "FIXED" ]];
then
    servTimeDist=0
elif [[ "$servTimeDist" == "NORMAL" ]];
then
    servTimeDist=1
elif [[ "$servTimeDist" == "UNIFORM" ]];
then
    servTimeDist=2
elif [[ "$servTimeDist" == "EXPONENTIAL" ]];
then
    servTimeDist=3
elif [[ "$servTimeDist" == "BIMODAL" ]];
then
    servTimeDist=4
else
    echo "Unsupported request distribution mode! Pick one from: FIXED; NORMAL; UNIFORM; EXPONENTIAL."
fi
'
#if [ ! -d $dirName ];
#then
#    mkdir $dirName
#fi

#if [ $servTimeDist == 4 ];then


for ratio in $ratios; do #15 25 50; do # 2 5 10 25 50 100;do
    for percent in $percents; do #30 ;do #1 5 10 25 50;do
        echo "hamed is here 1"
        #mkdir $dirName"_r"$ratio"_p"$percent
        for windowSize in 1; do
            for threadNum in 2 4 6; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d "new_data/"$dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        for windowSize in 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38 40 44 48 52; do
            for threadNum in 6; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d "new_data/"$dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        : '
        for windowSize in 8 12 16; do
            for threadNum in 5; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        for windowSize in 8 9 10 12 16; do
            for threadNum in 9; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        for windowSize in 12 16; do
            for threadNum in 13; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        for windowSize in 16; do
            for threadNum in 17 21 25 29; do
                for iter in 1; do
                    # echo $i
                    # date
                    ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                done
            done
        done
        '
    done
done
: '
    for ratio in $bimodal_disparity_ratio; do # 2 5 10 15 20 25;do
        for percent in $percent_long_queries; do #1 5 10 25 50;do
            mkdir $dirName"_r"$ratio"_p"$percent
            #rps=0
            #rps_prev=0
            for windowSize in 80 64 48 32 24 16 8 4 2;do #2 4 8 16 24 32 48 64 80;do
                for threadNum in 24;do
                        # echo $i
                        # date
                    for iter in 1; do
                        ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
		                #if [[ rps == 0 ]];then
                		#	rps=$(tail -n 1 runlog.txt)
		                #else
			            #    rps_prev=$rps
			            #    rps=$(tail -n 1 runlog.txt)
                            #if [[ rps -lt rps_prev*97/100 ]];then
				                #break 2
			                #fi
		                #fi
                    done
                done
            done
            for windowSize in 1; do
                for threadNum in 24 20 16 12 8 4;do #4 8 12 16 20 24; do
                    # echo $i
                    # date
                    for iter in 1; do
                        ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                    done
                done
            done
            for windowSize in 2;do #8 16 32 40 48; do
                for threadNum in 2; do
                    # echo $i
                    # date
                    for iter in 1; do
                        ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r $ratio -p $percent >> runlog_MT.txt
                    done
                done
            done
        done
    done
'
: '
else
    mkdir $dirName
    for windowSize in 1 2;do #8 16 32 40 48; do
        for threadNum in 1; do
            for iter in 1; do
                # echo $i
                # date
                ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r 0 -p 100 >> runlog_MT.txt
            done
        done
    done
    for windowSize in 1; do
        for threadNum in 4 8 10 12 14; do
            for iter in 1; do
                # echo $i
                # date
                ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r 0 -p 100 >> runlog_MT.txt
            done
        done
    done
    for windowSize in 1 2 4 8 12 16 20 24;do #28 32 36 40 44 48;do
        for threadNum in 16;do
            for iter in 1; do
                # echo $i
                # date
                ./mtclient -w $windowSize -j $threadNum -d $dirName"_r"$ratio"_p"$percent -v $ib_devname -g $gid -q $serverQpn -s $serverIPAddr -r 0 -p 100 >> runlog_MT.txt
                #if [[ rps == 0 ]];then
        		#	rps=$(tail -n 1 runlog.txt)
                #else
	            #    rps_prev=$rps
	            #    rps=$(tail -n 1 runlog.txt)
                    #if [[ rps -lt rps_prev*97/100 ]];then
		            #    exit
	                #fi
                #fi
            done
        done
    done
fi
'

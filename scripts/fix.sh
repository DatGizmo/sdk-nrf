#!/usr/bin/env bash

# This script merges the py-reqs from mcuboot, zephyr, and nrf.


OUT_FILE=$1
[ -z "$OUT_FILE" ] && echo "Error output file not provided" && exit 1

echo "#####       Generated Python Requirements             #####" > $OUT_FILE
echo "#####       Do not edit this file manually            #####" >> $OUT_FILE
echo "###########################################################" >> $OUT_FILE
echo "##### Merged inputs:                                  #####" >> $OUT_FILE
echo "#####     bootloader/mcuboot/scripts/requirements.txt #####" >> $OUT_FILE
echo "#####     zephyr/scripts/requirements.txt             #####" >> $OUT_FILE
echo "#####     nrf/scripts/requirements.txt                #####" >> $OUT_FILE
echo "" >> $OUT_FILE

source ~/.local/bin/virtualenvwrapper.sh
[[ $? != 0 ]] && echo "error sourcing virtualenvwrapper" && exit 1

rmvirtualenv pip-fixed-venv
mkvirtualenv pip-fixed-venv
workon pip-fixed-venv
pip3 install --isolated \
    -r ~/tmp/west/bootloader/mcuboot/scripts/requirements.txt \
    -r ~/tmp/west/zephyr/scripts/requirements.txt  \
    -r ./requirements.txt
pip3 freeze --all | LC_ALL=C sort --ignore-case >> $OUT_FILE
#Set LC_ALL=C to have the same sorting behaviour on all systems

deactivate
rmvirtualenv pip-fixed-venv

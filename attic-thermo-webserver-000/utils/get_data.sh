#!/bin/bash

. ./env.sh
epoch_secs=$(date +'%s')

wget -O - "http://$TEMP_HOST/f.txt?t=$epoch_secs"

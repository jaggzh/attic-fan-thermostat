#!/bin/bash

# Retrieve data from WiFi data server (thermostat+fan controller currently)
# env.sh in the same dir as this script should contain TEMP_HOST,
#  or you should have it otherwise loaded as an env var

# env.sh, in this script's dir, should contain:
# TEMP_HOST=ip_of_host
# STORE_DIR=/path/to/dir

##################################################################
# This is all just to get the script's directory, to load env.sh,
#  ... to get $TEMP_HOST and whatnot
# Courtesy of:
#  https://stackoverflow.com/questions/59895/how-to-get-the-source-directory-of-a-bash-script-from-within-the-script-itself
SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do # resolve $SOURCE until the file is no longer a symlink
  DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"
  SOURCE="$(readlink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE" # if $SOURCE was a relative symlink, we need to resolve it relative to the path where the symlink file was located
done
DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"

################################
# And here we load our settings
SETTINGS="$DIR/env.sh"
if [[ ! -e "$SETTINGS" ]]; then
	echo "WARNING: Missing $DIR/env.sh" >&2
	echo "We need \$TEMP_HOST. We might fail." >&2
else
	. "$SETTINGS"
fi

epoch_secs=$(date +'%s')
mkdir -p "$STORE_DIR"
data_file="$STORE_DIR/$(date +'%Y-%m-%d__%H_%M_%S.txt')"

echo "Writing to: $data_file" >&2
wget -O "$data_file" "http://$TEMP_HOST/f.txt?t=$epoch_secs"


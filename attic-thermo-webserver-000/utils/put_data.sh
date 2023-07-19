#!/bin/bash

# env.sh, in this script's dir, should contain:
# TEMP_HOST=ip_of_host
# STORE_DIR=/path/to/dir

SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do # resolve $SOURCE until the file is no longer a symlink
  DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"
  SOURCE="$(readlink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE" # if $SOURCE was a relative symlink, we need to resolve it relative to the path where the symlink file was located
done
DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"

# Load settings
SETTINGS="$DIR/env.sh"
if [[ ! -e "$SETTINGS" ]]; then
	echo "WARNING: Missing $DIR/env.sh" >&2
	echo "We need \$TEMP_HOST. We might fail." >&2
else
	. "$SETTINGS"
fi

# Get the most recent data file
data_file=$(ls -t "$STORE_DIR"/*.txt | head -n 1)

if [[ ! -e "$data_file" ]]; then
	echo "No data file found in $STORE_DIR" >&2
	exit 1
fi

echo "Sending data from: $data_file" >&2

# Filter out comment lines and send the data to the ESP8266
cat <<EOT
	grep -v '^#' "$data_file" | curl -X POST -d @- http://$TEMP_HOST/restoredata
EOT
echo "These are the lines we are sending:"
grep -v '^#' "$data_file" | head -n 20  | cut -f 2 | sed -e 's/^.*/80.00/;' # |  curl -X POST -d @- http://$TEMP_HOST/restoredata
echo "^^ These are the lines we are sending."
grep -v '^#' "$data_file" | head -n 20  | cut -f 2 | sed -e 's/^.*/80.00/;' | curl -X POST -d @- http://$TEMP_HOST/restoredata
#| curl -F "file=@foo" http://$TEMP_HOST/restoredata


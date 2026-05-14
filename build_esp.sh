#!/usr/bin/env bash
set -e

rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" all

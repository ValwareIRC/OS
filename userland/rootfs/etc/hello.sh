#!/bin/sh
echo "hello from a #! script, running as $(id -u 2>/dev/null || echo root)"

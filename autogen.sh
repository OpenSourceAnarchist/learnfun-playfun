#!/bin/sh

set -e
autoreconf --force --install --verbose "$@"

# After autoreconf, create a wrapper that supports the user-facing
# master flag forms: -all-opts or --all-opts. These are translated to
# the internal autoconf style --enable-all-opts.
if [ -f configure ]; then
	mv configure configure.real
	cat > configure <<'EOF'
#!/bin/sh
# Wrapper to provide portable master optimization flag aliases.
# Accepted forms:
#   -all-opts      Enable all optional performance optimizations.
#   --all-opts     Same as above (canonical short form without enable- prefix).
# Internally translated to: --enable-all-opts

norm_args=""
for arg in "$@"; do
	case "$arg" in
		-all-opts|--all-opts) norm_args="$norm_args --enable-all-opts" ;;
		*)                    norm_args="$norm_args $arg" ;;
	esac
done

exec "$(dirname "$0")/configure.real" $norm_args
EOF
	chmod +x configure
	echo "Created configure wrapper supporting -all-opts / --all-opts"
fi

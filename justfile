# Quick incremental compile in a build directory that ./minimed-build.sh already configured
# (build-asterix, build-pt2-slot0, build-pt2-slot1).
build dir="build-asterix":
	docker run --rm \
		-u "$(id -u):$(id -g)" \
		-e HOME=/tmp/home \
		-v "$PWD":/pebbleos \
		-w /pebbleos \
		pebbleos-minimed:v10 \
		bash -lc "mkdir -p \$HOME && git config --global --add safe.directory /pebbleos && \
			pbl build -b {{dir}}"



build:
	docker run --rm \
		-e HOME=/tmp \
		-v "$PWD":/pebbleos \
		-w /pebbleos \
		pebbleos-build:local \
		bash -lc "git config --global --add safe.directory /pebbleos && \
			export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH && \
			./waf build"

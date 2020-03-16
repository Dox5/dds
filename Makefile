.SILENT:

.PHONY: \
	docs docs-server docs-watch docs-sync-server nix-ci linux-ci macos-ci \
	vagrant-freebsd-ci

_invalid:
	echo "Specify a target name to execute"
	exit 1

docs:
	sphinx-build -b html \
		docs \
		_build/docs \
		-d _build/doctrees \
		-Wqanj8
	echo "Docs generated to _build/docs"

docs-server: docs
	echo "Docs are visible on http://localhost:9794/"
	cd _build/docs && \
		python -m http.server 9794

docs-watch: docs
	+sh tools/docs-watch.sh

docs-sync-server:
	mkdir -p _build/docs
	cd _build/docs && \
	browser-sync start --server \
		--reload-delay 300 \
		--watch **/*.html

macos-ci: nix-ci
linux-ci: nix-ci

nix-ci:
	python3 -u tools/ci.py \
		-B download \
		-T tools/gcc-9.dds \
		-T2 tools/gcc-9.jsonc

vagrant-freebsd-ci:
	vagrant up freebsd11
	vagrant ssh freebsd11 -c '\
		cd /vagrant && \
		python3.7 tools/ci.py \
			-B build \
			-T  tools/freebsd-gcc-9.dds \
			-T2 tools/freebsd-gcc-9.jsonc \
		'
	vagrant scp freebsd11:/vagrant/_build/dds _build/dds-freebsd-x64
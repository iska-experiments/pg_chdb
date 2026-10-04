# Named, not globbed: the control files of the other modules live here too.
EXTENSION    = chdb
# ctl_version, ext_module and libchdb_program, shared with the modules built
# against pg_chdb elsewhere.
include src/rules.mk
EXTVERSION   = $(call ctl_version,$(EXTENSION))
DISTVERSION  = $(shell grep -m 1 '^[[:space:]]\{2\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

MAX_CONCURRENT_TESTS ?=

DATA         = $(sort $(wildcard sql/$(EXTENSION)--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql)
DOCS         = $(wildcard doc/*.md)
TESTS        ?= $(wildcard test/sql/*.sql)
REGRESS      = --schedule test/schedule$(MAX_CONCURRENT_TESTS)
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION) $(if $(MAX_CONCURRENT_TESTS),--max-concurrent-tests $(MAX_CONCURRENT_TESTS))
MODULE_big   = $(EXTENSION)
TAP_TESTS   ?= 1
OBJS         = $(subst .c,.o, $(wildcard src/*.c))

# The programs linking libchdb, each built by a sub-make beside its sources.
HELPER       = src/helper/chdb_helper

# One jobserver sized to the machine's processors reaches every sub-make, so a
# bare make builds with every core; a -j on the command line still wins.
NPROC       ?= $(shell nproc --all 2>/dev/null || sysctl -n hw.ncpu)
MAKEFLAGS   += -j$(NPROC)

# Determine the OS and architecture.
OS         ?= $(shell uname -s | tr A-Z a-z)
ARCH        = $(shell uname -m)
ifeq ($(ARCH),aarch64)
  ARCH       := arm64
else ifeq ($(ARCH),x86_64)
  ARCH       := amd64
endif

CLANG_FORMAT ?= clang-format

# Binary dependency on specific version (for now) of libchdb. Optionally
# download locally by setting BUNDLE_LIBCHDB and compile statically with
# LIBCHDB_BUILD=static.
LIBCHDB_VERSION ?= v26.9.0
LIBCHDB_BUILD   ?= dynamic

# Clean up generated files.
EXTRA_CLEAN  = src/version.h sql/$(EXTENSION)--$(EXTVERSION).sql src/hook/chdb_hook$(DLSUFFIX) src/hook/*.o src/hook/*.bc test/schedule*

# The vendored headers, compiler flags and PGXS, shared with the modules below.
include src/module.mk

# Set default prove flags.
ifeq ($(PROVE_FLAGS),)
PROVE_FLAGS = -fwvj $(if $(MAX_CONCURRENT_TESTS),$(MAX_CONCURRENT_TESTS),$(NPROC))
endif

# Build against, install, uninstall a local copy of libchdb.
ifneq ($(BUNDLE_LIBCHDB),)
LIBCHDB_DIR = vendor/libchdb-$(LIBCHDB_VERSION)-$(OS)-$(ARCH)
$(HELPER): $(LIBCHDB_DIR)/lib/libchdb.$(if $(filter $(LIBCHDB_BUILD),static),a,so)
ifneq ($(LIBCHDB_BUILD),static)
install: install-libchdb
uninstall: uninstall-libchdb
endif
endif

# Require the versioned SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql src/hook/chdb_hook$(DLSUFFIX)

# The vendored headers are module.mk's; clickhouse.h first, as fetching it is a rule.
$(OBJS) $(OBJS:.o=.bc): $(CH_C_DIR)/clickhouse.h src/version.h $(wildcard src/*.h)

# Versioned SQL script.
sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Versioned source file.
src/version.h: META.json
	@printf '#define PGCHCB_VERSION "%s"\n' "$(DISTVERSION)" > $@

# Hook module.
HOOK_MODULE := src/hook/chdb_hook$(DLSUFFIX)
$(HOOK_MODULE): $(wildcard src/hook/*.c src/hook/*.h) $(OBJS)
	@$(MAKE) -C $(dir $@) all NO_FILE_SCHEME=$(NO_FILE_SCHEME)

# Install and uninstall the chdb_hook module.
install-hook: $(HOOK_MODULE)
	$(INSTALL_SHLIB) $< '$(DESTDIR)$(pkglibdir)/'
uninstall-hook:
	rm -f $(DESTDIR)$(pkglibdir)/$(HOOK_MODULE)
install: install-hook
uninstall: uninstall-hook

# Fail with something more useful than a missing include.
$(CH_C_DIR)/clickhouse.h: .gitmodules
	git submodule update --init --recursive

# chdb_helper answers one COPY.
$(eval $(call libchdb_program,helper,$(HELPER),src/setup.h))

# What extensions built against pg_chdb link and include: the objects every
# module here links but chdb.o, archived as libpgchdb.a into pkglibdir, and
# the headers, the Makefiles they are built with and the vendored headers,
# under the server's include directory in extension/chdb, where PGXS's
# HEADERS would put a module's own. chdb.mk names where they went.
CHDB_LIB       := src/libpgchdb.a
CHDB_HEADERS   := $(addprefix src/,channel.h gucs.h helper.h module.h native.h \
                  native_insert.h native_writer.h setup.h spawn.h srf.h)
CHDB_MAKEFILES := $(addprefix src/,libchdb.mk module.mk rules.mk)
chdb_incdir     = $(includedir_server)/extension/chdb
$(CHDB_LIB): $(filter-out src/chdb.o,$(OBJS))
	rm -f $@ && $(AR) $(AROPT) $@ $^
all: $(CHDB_LIB)
EXTRA_CLEAN += $(CHDB_LIB)

install-headers: $(CHDB_LIB)
	$(MKDIR_P) '$(DESTDIR)$(chdb_incdir)/vendor/pg-clickhouse-c/clickhouse-c'
	$(INSTALL_DATA) $(CHDB_HEADERS) $(CHDB_MAKEFILES) '$(DESTDIR)$(chdb_incdir)/'
	$(INSTALL_DATA) $(wildcard $(PGCH_DIR)/*.h) '$(DESTDIR)$(chdb_incdir)/vendor/pg-clickhouse-c/'
	$(INSTALL_DATA) $(wildcard $(CH_C_DIR)/*.h) '$(DESTDIR)$(chdb_incdir)/vendor/pg-clickhouse-c/clickhouse-c/'
	sed -e 's|@VERSION@|$(DISTVERSION)|' -e 's|@INCLUDEDIR@|$(chdb_incdir)|' \
	    -e 's|@PKGLIBDIR@|$(pkglibdir)|' src/chdb.mk.in > '$(DESTDIR)$(chdb_incdir)/chdb.mk'
	$(INSTALL_STLIB) $(CHDB_LIB) '$(DESTDIR)$(pkglibdir)/'
uninstall-headers:
	rm -rf '$(DESTDIR)$(chdb_incdir)' '$(DESTDIR)$(pkglibdir)/$(notdir $(CHDB_LIB))'
install: install-headers
uninstall: uninstall-headers
.PHONY: install-headers uninstall-headers

.PHONY: test/schedule$(MAX_CONCURRENT_TESTS)
test/schedule$(MAX_CONCURRENT_TESTS): schedule = $(if $(TESTS),$(patsubst test/sql/%.sql,%,$(TESTS)),)
test/schedule$(MAX_CONCURRENT_TESTS):
ifneq ($(MAX_CONCURRENT_TESTS),)
	@perl -E 'say "test: ", join " ", splice @ARGV, 0, $(MAX_CONCURRENT_TESTS) while @ARGV' $(schedule) > $@
else
	@echo $(if $(schedule),test: $(schedule),) > $@
endif

installcheck: test/schedule$(MAX_CONCURRENT_TESTS)

# libchdb
$(LIBCHDB_DIR)/lib/libchdb.so:
	@env INSTALL_VERSION="$(LIBCHDB_VERSION)" DESTDIR=$(LIBCHDB_DIR) bash vendor/get-libchdb.sh

$(LIBCHDB_DIR)/lib/libchdb.a:
	env INSTALL_VERSION="$(LIBCHDB_VERSION)" DESTDIR=$(LIBCHDB_DIR) STATIC=1 bash vendor/get-libchdb.sh

# GitHub stuff.
libchdb-version:
	@echo $(LIBCHDB_VERSION)
libchdb-variables:
	@echo VERSION=$(LIBCHDB_VERSION)
	@echo DIRECTORY=$(LIBCHDB_DIR)

# Install and uninstall libchdb, which is configured to live in /usr/local/lib.
install-libchdb: $(LIBCHDB_DIR)/lib/libchdb.so
	$(MKDIR_P) $(DESTDIR)/usr/local/lib
	$(INSTALL_SHLIB) $< $(DESTDIR)/usr/local/lib
	if [ "$$(uname -s)" = "Linux" ]; then ldconfig; fi

uninstall-libchdb:
	rm -f $(DESTDIR)/usr/local/lib/libchdb.so

.PHONY: format # Format .c and .h files to project standard in .clang-format.
format: $(wildcard src/*.c src/*.h src/helper/*.c)
	@$(CLANG_FORMAT) --style=file:.clang-format -i $^

.PHONY: type-table # Regenerate the data type tables of doc/chdb_hook.md.
type-table:
	@(cd $(PGCH_DIR) && ./gen_type_table.awk) | dev/type_table.awk doc/chdb_hook.md
	@(cd $(PGCH_DIR) && ./gen_type_table.awk -v section=ENCODE) | \
		dev/type_table.awk -v section=ENCODE doc/chdb_hook.md

.PHONY: clang-tidy # Run clang-tidy static analysis (requires compile_commands.json)
clang-tidy: compile_commands.json
	run-clang-tidy -p . $(wildcard src/*.c src/*.h src/helper/*.c)

.PHONY: lint # Lint the project
lint: .pre-commit-config.yaml
	@pre-commit run --show-diff-on-failure --color=always --all-files

## .git/hooks/pre-commit: Install the pre-commit hook
.git/hooks/pre-commit:
	@printf "#!/bin/sh\nmake lint\n" > $@
	@chmod +x $@

# Requires https://github.com/rizsotto/Bear.
compile_commands.json:
	$(MAKE) clean -j $$(nproc)
	bear --config "dev/bear.$$(if [ "$$(bear --version | awk -F'[^0-9]+' '{ print $$2 }')" -eq 3 ]; then echo 'json'; else echo 'yml'; fi)" -- $(MAKE) all -j $$(nproc)

debian-install-lint:
	@curl -SsLo /tmp/pre-commit.pyz https://github.com/pre-commit/pre-commit/releases/download/v4.6.0/pre-commit-4.6.0.pyz
	@printf "#!/bin/sh\npython3 /tmp/pre-commit.pyz \"\$$@\"\n" > /usr/local/bin/pre-commit
	@chmod +x /usr/local/bin/pre-commit

# Test the PGXN distribution.
dist-test: $(EXTENSION)-$(DISTVERSION).zip
	unzip $(EXTENSION)-$(DISTVERSION).zip
	cd $(EXTENSION)-$(DISTVERSION)
	$(MAKE) && $(DIST_TEST_SUDO) $(MAKE) install && $(MAKE) installcheck

.PHONY: release-notes # Show release notes for current version (must have `mknotes` in PATH).
release-notes: CHANGELOG.md
	mknotes -v v$(DISTVERSION) -f $< -r https://github.com/$(or $(GITHUB_REPOSITORY),ClickHouse/pg_chdb)

$(EXTENSION)-$(DISTVERSION).zip:
	git archive-all -v --prefix "$(EXTENSION)-$(DISTVERSION)/" --force-submodules $(EXTENSION)-$(DISTVERSION).zip

zip: $(EXTENSION)-$(DISTVERSION).zip

kv-rest:
	curl -Ls https://github.com/theory/kv-rest/releases/download/v0.1.1/kv-rest-v0.1.1-$(OS)-$(ARCH).tar.gz | tar zxf - --strip-components=1 $(if $(filter $(OS),linux),--wildcards) '*/kv-rest'

start-kv-rest: kv-rest
	KVREST_PORT="$${KVREST_PORT:-9182}" ./kv-rest &

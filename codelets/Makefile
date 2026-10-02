# codelets/Makefile — build and verify every codelet in this directory.
#
#   make                  build + verify all codelets
#   make verify           re-verify existing objects
#   make import-contract  copy ocudu's ctx header into include/ (for builds
#                         without an ocudu checkout, e.g. a container)
#   make check-contract   fail if the imported copy has drifted from ocudu
#   make clean

include Makefile.defs

.PHONY: all verify clean cleanall import-contract check-contract $(CODELET_DIRS)

all: check-env check-contract $(CODELET_DIRS)

$(CODELET_DIRS):
	$(MAKE) -C $@

verify:
	@for d in $(CODELET_DIRS); do $(MAKE) -C $$d verify || exit 1; done

clean cleanall:
	@for d in $(CODELET_DIRS); do $(MAKE) -C $$d $@ || exit 1; done

# ---- ctx contract (A2a) ----
#
# ocudu is the source of truth: include/ocudu/janus/jbpf_srsran_contexts.h.
# `import-contract` takes a copy for environments where that tree is absent;
# `check-contract` is a no-op there and a hard diff where it is present, so
# drift surfaces as a build failure instead of an ABI mismatch at runtime.

CONTRACT_HDR := jbpf_srsran_contexts.h

import-contract:
	@if [ ! -f "$(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" ]; then \
		echo "ERROR: $(OCUDU_JANUS_INC)/$(CONTRACT_HDR) not found." >&2; \
		echo "       Pass OCUDU_DIR=/path/to/ocudu-e3" >&2; \
		exit 1; \
	fi
	@mkdir -p $(CONTRACT_INC)
	cp "$(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" "$(CONTRACT_INC)/$(CONTRACT_HDR)"
	@echo "imported $(CONTRACT_HDR) from $(OCUDU_JANUS_INC)"

check-contract:
	@if [ ! -f "$(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" ]; then \
		if [ -f "$(CONTRACT_INC)/$(CONTRACT_HDR)" ]; then \
			echo "note: no ocudu tree at $(OCUDU_DIR); using imported $(CONTRACT_HDR)"; \
		else \
			echo "ERROR: no ctx contract available." >&2; \
			echo "       Either pass OCUDU_DIR=/path/to/ocudu-e3," >&2; \
			echo "       or run 'make import-contract' where that tree exists." >&2; \
			exit 1; \
		fi; \
	elif [ -f "$(CONTRACT_INC)/$(CONTRACT_HDR)" ] && \
	     ! diff -q "$(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" "$(CONTRACT_INC)/$(CONTRACT_HDR)" >/dev/null; then \
		echo "ERROR: ctx contract drift." >&2; \
		echo "  ocudu:    $(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" >&2; \
		echo "  imported: $(CONTRACT_INC)/$(CONTRACT_HDR)" >&2; \
		diff -u "$(CONTRACT_INC)/$(CONTRACT_HDR)" "$(OCUDU_JANUS_INC)/$(CONTRACT_HDR)" >&2 || true; \
		echo "Run 'make import-contract' to resync, then rebuild ALL codelets:" >&2; \
		echo "a changed ctx layout shifts every offset they read through." >&2; \
		exit 1; \
	fi

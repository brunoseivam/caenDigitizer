TOP=..
include $(TOP)/configure/CONFIG
include $(TOP)/configure/RULES

$(INSTALL_INCLUDE)/nlohmann/json.hpp: $(TOP)/jsonApp/json/single_include/nlohmann/json.hpp
	@[ -d $(dir $@) ] || install -d $(dir $@)
	install -m 644 $< $@

$(INSTALL_INCLUDE)/nlohmann/json_fwd.hpp: $(TOP)/jsonApp/json/single_include/nlohmann/json_fwd.hpp
	@[ -d $(dir $@) ] || install -d $(dir $@)
	install -m 644 $< $@

install: $(INSTALL_INCLUDE)/nlohmann/json.hpp $(INSTALL_INCLUDE)/nlohmann/json_fwd.hpp

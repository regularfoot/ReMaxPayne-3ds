DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CC_HOST ?= cc
HOST_FLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pedantic
.DEFAULT_GOAL := native
.PHONY: native test

native:
	DEVKITPRO="$(DEVKITPRO)" DEVKITARM="$(DEVKITARM)" python3 scripts/build_native.py
	@mkdir -p dist/native/3ds/maxpayne
	cp build/native/maxpayne-native.3dsx dist/native/3ds/maxpayne/maxpayne.3dsx

build/host/test-native-scene: tests/test_native_scene.c source/native/portal.c source/native/portal.h source/native/scene.c source/native/scene.h source/native/room.h source/native/touch.c source/native/touch.h
	@mkdir -p build/host
	$(CC_HOST) $(HOST_FLAGS) tests/test_native_scene.c source/native/scene.c source/native/touch.c source/native/portal.c -lm -o $@

build/host/test-campaign-ballistics: tests/test_campaign_ballistics.c source/native/campaign_combat.c source/native/campaign_combat.h source/native/campaign_world.h source/native/director.h source/native/scene.c source/native/scene.h source/native/particle_layer.h
	@mkdir -p build/host
	$(CC_HOST) $(HOST_FLAGS) tests/test_campaign_ballistics.c source/native/campaign_combat.c source/native/scene.c -lm -o $@

build/host/test-native-nav: tests/test_native_nav.c source/native/campaign_nav.c source/native/campaign_nav.h source/native/director.h
	@mkdir -p build/host
	$(CC_HOST) $(HOST_FLAGS) tests/test_native_nav.c -lm -o $@

test: build/host/test-native-scene build/host/test-campaign-ballistics build/host/test-native-nav
	./build/host/test-native-scene
	./build/host/test-campaign-ballistics
	./build/host/test-native-nav

.PHONY: native-full
GAME_DISC ?= game
CAMPAIGN_CACHE ?= build/campaign-source
CAMPAIGN_DIR ?= dist/native/3ds/maxpayne/campaign
ASSET_PYTHON ?= python3
CAMPAIGN_TEXTURE_LIMIT ?= 512

.PHONY: campaign-assets
campaign-assets:
	$(ASSET_PYTHON) scripts/campaign_source.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)"
	$(ASSET_PYTHON) scripts/pack_campaign.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)" --texture-limit $(CAMPAIGN_TEXTURE_LIMIT)
	$(ASSET_PYTHON) scripts/pack_campaign_events.py "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)" --disc "$(GAME_DISC)"
	$(ASSET_PYTHON) scripts/pack_campaign_rules.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_bounds.py "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_room_surfaces.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_nav.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_media.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_audio.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)/audio.bank"
	$(ASSET_PYTHON) scripts/pack_campaign_cast.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_faces.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_cameras.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)/cameras.pak"
	$(ASSET_PYTHON) scripts/pack_campaign_poses.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_cues.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_items.py "$(GAME_DISC)" "$(CAMPAIGN_CACHE)" "$(CAMPAIGN_DIR)"
	$(ASSET_PYTHON) scripts/pack_campaign_particles.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)/particles.pak"
	$(ASSET_PYTHON) scripts/pack_campaign_projectiles.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)/projectiles.bin"
	$(ASSET_PYTHON) scripts/pack_campaign_moves.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)/moves.anim"
	$(ASSET_PYTHON) scripts/pack_campaign_movies.py "$(GAME_DISC)" "$(CAMPAIGN_DIR)"

native-full: native
	DEVKITPRO="$(DEVKITPRO)" GAME_DISC="$(GAME_DISC)" $(ASSET_PYTHON) scripts/package_campaign.py "$(CAMPAIGN_DIR)" dist/native-full/3ds/maxpayne/maxpayne.3dsx

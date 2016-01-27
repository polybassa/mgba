/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#ifdef USE_CLI_DEBUGGER
#include "debugger/cli-debugger.h"
#endif

#ifdef USE_GDB_STUB
#include "debugger/gdb-stub.h"
#endif

#include "core/core.h"
#ifdef M_CORE_GBA
#include "gba/gba.h"
#include "gba/context/config.h"
#include "gba/supervisor/thread.h"
#include "gba/video.h"
#endif
#ifdef M_CORE_GB
#include "gb/core.h"
#include "gb/gb.h"
#include "gb/video.h"
#endif
#include "platform/commandline.h"
#include "util/configuration.h"
#include "util/vfs.h"

#include <SDL.h>

#include <errno.h>
#include <signal.h>
#include <sys/time.h>

#define PORT "sdl"

// TODO: Move somewhere
enum mPlatform {
	PLATFORM_NONE = -1,
	PLATFORM_GBA,
	PLATFORM_GB
};

static bool mSDLInit(struct mSDLRenderer* renderer);
static void mSDLDeinit(struct mSDLRenderer* renderer);

// TODO: Clean up signatures
#ifdef M_CORE_GBA
static int mSDLRunGBA(struct mSDLRenderer* renderer, struct GBAArguments* args, struct GBAOptions* opts, struct GBAConfig* config);
#endif
#ifdef M_CORE_GB
static int mSDLRunGB(struct mSDLRenderer* renderer, struct GBAArguments* args);
#endif


int main(int argc, char** argv) {
	struct mSDLRenderer renderer;

	struct GBAInputMap inputMap;
	GBAInputMapInit(&inputMap);

	struct GBAConfig config;
	GBAConfigInit(&config, PORT);
	GBAConfigLoad(&config);

	struct GBAOptions opts = {
		.width = 0,
		.height = 0,
		.useBios = true,
		.rewindEnable = true,
		.audioBuffers = 512,
		.videoSync = false,
		.audioSync = true,
	};

	struct GBAArguments args;
	struct GraphicsOpts graphicsOpts;

	struct SubParser subparser;

	initParserForGraphics(&subparser, &graphicsOpts);
	bool parsed = parseArguments(&args, &config, argc, argv, &subparser);
	if (!parsed || args.showHelp) {
		usage(argv[0], subparser.usage);
		freeArguments(&args);
		GBAConfigFreeOpts(&opts);
		GBAConfigDeinit(&config);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		freeArguments(&args);
		GBAConfigFreeOpts(&opts);
		GBAConfigDeinit(&config);
		return 0;
	}

	enum mPlatform platform = PLATFORM_NONE;

	if (args.fname) {
		struct VFile* vf = VFileOpen(args.fname, O_RDONLY);
		if (!vf) {
			printf("Could not open game. Are you sure the file exists?\n");
			freeArguments(&args);
			GBAConfigFreeOpts(&opts);
			GBAConfigDeinit(&config);
			return 1;
		}
#ifdef M_CORE_GBA
		else if (GBAIsROM(vf)) {
			platform = PLATFORM_GBA;
			if (!opts.width) {
				opts.width = VIDEO_HORIZONTAL_PIXELS;
			}
			if (!opts.height) {
				opts.height = VIDEO_VERTICAL_PIXELS;
			}
			GBAVideoSoftwareRendererCreate(&renderer.d);
#ifdef BUILD_GL
			mSDLGLCreate(&renderer);
#elif defined(BUILD_GLES2) || defined(USE_EPOXY)
			mSDLGLES2Create(&renderer);
#else
			mSDLSWCreate(&renderer);
#endif
		}
#endif
#ifdef M_CORE_GB
		else if (GBIsROM(vf)) {
			platform = PLATFORM_GB;
			if (!opts.width) {
				opts.width = /*GB_*/VIDEO_HORIZONTAL_PIXELS;
			}
			if (!opts.height) {
				opts.height = /*GB_*/VIDEO_VERTICAL_PIXELS;
			}
			renderer.core = GBCoreCreate();
#ifdef BUILD_GL
			mSDLGLCreateGB(&renderer);
#elif defined(BUILD_GLES2) || defined(USE_EPOXY)
			mSDLGLES2CreateGB(&renderer);
#else
			mSDLSWCreateGB(&renderer);
#endif
		}
#endif
		else {
			printf("Could not run game. Are you sure the file exists and is a Game Boy Advance game?\n");
			freeArguments(&args);
			GBAConfigFreeOpts(&opts);
			GBAConfigDeinit(&config);
			return 1;
		}
	}

	GBAConfigLoadDefaults(&config, &opts);
	GBAConfigMap(&config, &opts);

	renderer.viewportWidth = opts.width;
	renderer.viewportHeight = opts.height;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	renderer.player.fullscreen = opts.fullscreen;
	renderer.player.windowUpdated = 0;
#else
	renderer.fullscreen = opts.fullscreen;
#endif
	renderer.ratio = graphicsOpts.multiplier;
	if (renderer.ratio == 0) {
		renderer.ratio = 1;
	}

	renderer.lockAspectRatio = opts.lockAspectRatio;
	renderer.filter = opts.resampleVideo;

	if (!mSDLInit(&renderer)) {
		freeArguments(&args);
		GBAConfigFreeOpts(&opts);
		GBAConfigDeinit(&config);
		return 1;
	}

	if (renderer.core) {
		// TODO: Check return code
		renderer.core->init(renderer.core);
	}

	renderer.player.bindings = &inputMap;
	GBASDLInitBindings(&inputMap);
	GBASDLInitEvents(&renderer.events);
	GBASDLEventsLoadConfig(&renderer.events, GBAConfigGetInput(&config));
	GBASDLAttachPlayer(&renderer.events, &renderer.player);
	GBASDLPlayerLoadConfig(&renderer.player, GBAConfigGetInput(&config));

	int ret;

	switch (platform) {
	case PLATFORM_GBA:
		ret = mSDLRunGBA(&renderer, &args, &opts, &config);
		break;
	case PLATFORM_GB:
		ret = mSDLRunGB(&renderer, &args);
		break;
	default:
		ret = 1;
		break;
	}
	GBASDLDetachPlayer(&renderer.events, &renderer.player);
	GBAInputMapDeinit(&inputMap);

	mSDLDeinit(&renderer);

	freeArguments(&args);
	GBAConfigFreeOpts(&opts);
	GBAConfigDeinit(&config);

	return ret;
}

#ifdef M_CORE_GBA
int mSDLRunGBA(struct mSDLRenderer* renderer, struct GBAArguments* args, struct GBAOptions* opts, struct GBAConfig* config) {
	struct GBAThread context = {
		.renderer = &renderer->d.d,
		.userData = renderer
	};

	context.debugger = createDebugger(args, &context);

	GBAMapOptionsToContext(opts, &context);
	GBAMapArgumentsToContext(args, &context);
	context.overrides = GBAConfigGetOverrides(config);

	bool didFail = false;

	renderer->audio.samples = context.audioBuffers;
	renderer->audio.sampleRate = 44100;
	if (opts->sampleRate) {
		renderer->audio.sampleRate = opts->sampleRate;
	}
	if (!GBASDLInitAudio(&renderer->audio, &context)) {
		didFail = true;
	}

	if (!didFail) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
		GBASDLSetScreensaverSuspendable(&renderer->events, opts->suspendScreensaver);
		GBASDLSuspendScreensaver(&renderer->events);
#endif
		if (GBAThreadStart(&context)) {
			renderer->runloop(renderer, &context);
			GBAThreadJoin(&context);
		} else {
			didFail = true;
			printf("Could not run game. Are you sure the file exists and is a Game Boy Advance game?\n");
		}

#if SDL_VERSION_ATLEAST(2, 0, 0)
		GBASDLResumeScreensaver(&renderer->events);
		GBASDLSetScreensaverSuspendable(&renderer->events, false);
#endif

		if (GBAThreadHasCrashed(&context)) {
			didFail = true;
			printf("The game crashed!\n");
		}
	}
	free(context.debugger);
	GBADirectorySetDeinit(&context.dirs);

	return didFail;
}
#endif

#ifdef M_CORE_GB
int mSDLRunGB(struct mSDLRenderer* renderer, struct GBAArguments* args) {
	struct VFile* vf = VFileOpen(args->fname, O_RDONLY);
	struct VFile* savVf = 0;

	{
		char savepath[PATH_MAX];
		char dirname[PATH_MAX];
		char basename[PATH_MAX];
		separatePath(args->fname, dirname, basename, 0);
		snprintf(savepath, sizeof(savepath), "%s" PATH_SEP "%s.sav", dirname, basename);
		savVf = VFileOpen(savepath, O_RDWR | O_CREAT);
	}

	renderer->core->loadROM(renderer->core, vf, savVf, args->fname);
	renderer->core->reset(renderer->core);
	renderer->runloop(renderer, NULL);
	renderer->core->unloadROM(renderer->core);
	vf->close(vf);
	return 0;
}
#endif

static bool mSDLInit(struct mSDLRenderer* renderer) {
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("Could not initialize video: %s\n", SDL_GetError());
		return false;
	}

	return renderer->init(renderer);
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	GBASDLDeinitEvents(&renderer->events);
	GBASDLDeinitAudio(&renderer->audio);
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_DestroyWindow(renderer->window);
#endif

	renderer->deinit(renderer);

	SDL_Quit();
}

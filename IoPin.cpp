/***************************************************************************
 * Artekit Wavetooeasy
 * https://www.artekit.eu/products/devboards/wavetooeasy
 *
   Written by Ivan Meleca
 * Copyright (c) 2021 Artekit Labs
 * https://www.artekit.eu

### IoPin.cpp

#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.

***************************************************************************/

#include "Debug.h"
#include "IoPin.h"
#include "Player.h"

extern PlayersPool players;

IoPin::IoPin(uint8_t num, char* file, PinPolarity polarity, PinTriggerType trigger,
		  PlayMode playback, float volume, DeassertMode deassert, uint32_t debounce, uint32_t max_random) :

	      player(NULL), pin_num(num), enabled(false), state(PinDeasserted),
		  last_state(PinDeasserted), error(false), deassert_mode(deassert),
		  io_polarity(polarity), trigger_type(trigger), playback_mode(playback), volume(volume),
		  max_random_index(max_random),
		  debounce(debounce), debouncer_state(PinDeasserted)
{
	if (file)
		strcpy(file_path, file);
}

bool IoPin::begin()
{
	// Initialize pin
	pinMode(pin_num, (io_polarity == PinActiveHigh) ? INPUT_PULLDOWN : INPUT_PULLUP);
	attachInterruptWithParam(pin_num, triggeredStub, CHANGE, this);

	// If trigger type is 'level', check the current level and act accordingly
	if (trigger_type == LevelTrigger)
	{
		uint8_t level = digitalRead(pin_num);
		if (io_polarity == PinActiveHigh)
			state = (level) ? PinAsserted : PinDeasserted;
		else
			state = (!level) ? PinAsserted : PinDeasserted;
	}

	enabled = true;

	debugMsg(DebugInfo, "Pin %i enabled (%s, %s, %s, max_random=%u)" ,
				 pin_num, io_polarity == PinActiveHigh ? "IoActiveHigh" : "IoActiveLow",
						 trigger_type == LevelTrigger ? "LevelTrigger" : "EdgeTrigger",
						 file_path, max_random_index);
	return true;
}

void IoPin::end()
{
	detachInterrupt(pin_num);

	// Set pin pull-up
	digitalWrite(pin_num, HIGH);
}

void IoPin::triggered()
{
	if (!enabled)
		return;

	// This function is called when an interrupt is triggered. Since we can
	// set different trigger modes and each of them follows a certain logic,
	// we just set a variable with the last state. No audio files operations
	// are done within this function.

	uint8_t level = digitalRead(pin_num);
	if (io_polarity == PinActiveHigh)
		state = (level) ? PinAsserted : PinDeasserted;
	else
		state = (!level) ? PinAsserted : PinDeasserted;

	// Check if there is a debouncer setting
	if (debounce)
	{
		if (debouncer_state != state)
		{
			debouncer_state = state;
			debouncer.startCounter();
		}
	}
}

void IoPin::getPlayFilePath(char* out_buffer, size_t buffer_size)
{
	if (max_random_index == 0)
	{
		snprintf(out_buffer, buffer_size, "%s", file_path);
		return;
	}

	// Seed the random number generator if not already seeded
	static bool rng_seeded = false;
	if (!rng_seeded)
	{
		randomSeed(micros());
		rng_seeded = true;
	}

	// Add the a random index to the file name, right before ".wav" so
	// it becomes "filename.N.wav".
	uint32_t rand_idx = random(1, max_random_index + 1);
	int base_len = (int)strlen(file_path) - 4;
	snprintf(out_buffer, buffer_size, "%.*s.%u%s", base_len, file_path, rand_idx, file_path + base_len);
}

inline void IoPin::processEdgeAsserted()
{
	if (!player)
	{
		// Try to get a free player
		player = players.acquire();
		if (!player)
		{
			debugMsg(DebugWarning, "Pin %i player not available", pin_num);
			return;
		}
	}

	char target_file[256];
	getPlayFilePath(target_file, sizeof(target_file));

	switch (player->getStatus())
	{
		case playerPlaying:
			switch (deassert_mode)
			{
				case DeassertPause:
					player->pause(true);
					debugMsg(DebugInfo, "Pin %i paused", pin_num);
					break;

				case DeassertRestart:
					if (!player->play(target_file, playback_mode))
					{
						debugMsg(DebugError, "Pin %i - error re-playing %s", pin_num, target_file);
						players.release(player);
						player = NULL;
						error = true;
					} else {
						debugMsg(DebugInfo, "Pin %i re-playing %s", pin_num, target_file);
					}
					break;

				case DeassertStop:
					player->stop();
					players.release(player);
					player = NULL;
					break;
			}
			break;

		case playerPaused:
			// deassert_mode should be DeassertPause
            player->resume();
			debugMsg(DebugInfo, "Pin %i resumed", pin_num);
			break;

		case playerStopped:
			if (!player->play(target_file, playback_mode))
			{
				debugMsg(DebugError, "Pin %i - error playing %s", pin_num, target_file);
				players.release(player);
				player = NULL;
				error = true;
			} else {
				debugMsg(DebugInfo, "Pin %i playing %s", pin_num, target_file);
			}
			break;
	}
}

inline void IoPin::processLevelAsserted()
{
	if (!player)
	{
		// Try to get a free player
		player = players.acquire();
		if (!player)
		{
			debugMsg(DebugWarning, "Pin %i processLevelAsserted() player not available", pin_num);
			return;
		}

        player->setVolume(volume);
	}

	if (player->getStatus() == playerPlaying)
		return;

	if (player->getStatus() == playerPaused)
	{
        player->resume();
		debugMsg(DebugInfo, "Pin %i resumed", pin_num);
		return;
	}

	char target_file[256];
	getPlayFilePath(target_file, sizeof(target_file));

	if (!player->play(target_file, playback_mode))
	{
		debugMsg(DebugError, "Pin %i - error playing %s", pin_num, target_file);
        players.release(player);
		player = NULL;
		error = true;
	} else {
		debugMsg(DebugInfo, "Pin %i playing %s", pin_num, target_file);
	}
}

inline void IoPin::processLevelDeasserted()
{
	if (!player)
	{
		debugMsg(DebugWarning, "Pin %i processLevelDeasserted() without player", pin_num);
		return;
	}

	if (player->getStatus() != playerPlaying)
	{
		debugMsg(DebugWarning, "Pin %i processLevelDeasserted() not playing", pin_num);
		return;
	}

	if (deassert_mode == DeassertPause && player->getStatus() == playerPlaying)
	{
        player->pause(true);
		debugMsg(DebugInfo, "Pin %i paused", pin_num);
	} else {
        player->stop(true);
		debugMsg(DebugInfo, "Pin %i stopped", pin_num);
	}
}

bool IoPin::poll()
{
	// This error flag prevents the player from keep trying to read a file that
	// may not exist, or can't be played for other reasons (i.e. bad format).
	if (error || !enabled)
		return false;

    // Free and invalidate the player if we are not playing
	if (player && player->getStatus() == playerStopped)
    {
        players.release(player);
		player = NULL;
    }

	PinState read_state = state;

	// Check if there is an active debouncer
	if (debouncer.active())
	{
		if (debouncer.elapsed() < debounce)
			return false;

		debouncer.stopCounter();
	}

	if (last_state == read_state)
		return false;

	last_state = read_state;

	debugMsg(DebugInfo, "Pin %i %s",
			 pin_num, read_state == PinAsserted ? "asserted" : "deasserted");

	// Edge-trigger mode
	if (trigger_type == EdgeTrigger)
	{
		if (read_state == PinAsserted)
			processEdgeAsserted();
	} else if (trigger_type == LevelTrigger)
	{
		if (read_state == PinAsserted)
			processLevelAsserted();
		else
			processLevelDeasserted();
	}

	return true;
}

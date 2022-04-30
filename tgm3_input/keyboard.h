#pragma once

#include "base_input.h"
#include <list>
#include <map>

class keyboard : public base_input {
	// TGM3 buttons
	mutable unsigned short buttons;

	// Buttons from the previous time buttons were fetched
	// (Just ABCD buttons, not directional ones)
	mutable unsigned short prev_buttons;

	mutable unsigned short pending_release_buttons;

	// Directional buttons from the previous update
	unsigned short old_dir_buttons;

	// List containing which direction keys are pressed
	std::list<unsigned short> direction_keys;

	// Virtual key -> Button mask table
	unsigned short key_mask_map[0xFF];

public:
	// Raw input usage
	std::vector<int> get_usage() override
	{
		return { { 6 } };
	}

	// Load keycodes from config
	void init(const config &cfg) override;

	// Update direction_keys and buttons
	void update(const tagRAWINPUT *input) override;

	// Clear direction keys list too
	void clear_buttons() override
	{
		buttons = 0;
		prev_buttons = 0;
		direction_keys.clear();
	}

	// buttons accessors
	unsigned short get_buttons_1p() const override
	{
		unsigned short out_buttons = buttons;

		prev_buttons = buttons;
		buttons &= ~pending_release_buttons;

		pending_release_buttons = 0;

		return out_buttons;
	}

	unsigned short get_buttons_2p() const override
	{
		return 0;
	}
};
// Copyright Microsoft and CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "identifier.h"
#include <debug.hh>
#include <fail-simulator-on-error.h>
#include <token.h>

using Debug = ConditionalDebug<true, "Identifier service">;

/**
 * A simple opaque types.  Callers to this service can hold sealed pointers to
 * this structure but they cannot ever access its contents.
 */
struct Identifier
{
	int value;
};

/**
 * Helper to get the key used for the allocator.
 */
static auto key()
{
	static auto key = token_key_new();
	return key;
}

/**
 * Create a new identifier holding the specified value.
 */
Identifier *identifier_create(int value)
{
	// Allocate the identifier object and get back both sealed and unsealed
	// capabilities.
	auto [unsealed, sealed] = token_allocate<Identifier>(key());
	if (sealed == nullptr)
	{
		return nullptr;
	}
	Debug::log(
	  "Allocated identifier, sealed capability: {}\nunsealed capability: {}",
	  sealed.get(),
	  unsealed);
	unsealed->value = value;
	return sealed.get();
}

/**
 * Returns the value held in a identifier.
 */
int identifier_value(Identifier *identifier)
{
	// Unseal the identifier.
	auto *unsealedIdentifier =
	  token_unseal(key(), Sealed<Identifier>{identifier});
	// If this is not a valid identifier, the call above will return nullptr,
	// any other value indicates that the identifier is valid.
	if (unsealedIdentifier != nullptr)
	{
		return unsealedIdentifier->value;
	}
	return 0;
}

/**
 * Destroy the identifier provided as an argument.
 */
void identifier_destroy(Identifier *identifier)
{
	// The allocator does validity checks here, so we can skip them.
	token_obj_destroy(key(), reinterpret_cast<SObj>(identifier));
}

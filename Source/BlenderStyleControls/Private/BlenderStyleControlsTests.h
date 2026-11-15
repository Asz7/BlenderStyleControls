#pragma once

/**
 * Dedicated-process integration check. Refuses to run without
 * -BlenderControlsVerify, creates an unsaved blank editor world, writes a JSON
 * report, and requests process exit. Never call in the user's working session.
 */
void RunBlenderStyleControlsVerification();

#include "PianoRollModule.hpp"

using namespace rack;

static const float PLUGGED_GATE_DURATION = std::numeric_limits<float>::max();
static const float AUDITION_GATE_DURATION = std::numeric_limits<float>::max();
static const float UNPLUGGED_GATE_DURATION = 2.0f;


PianoRollModule::PianoRollModule() : BaseModule(), runInputActive(false), transport(&patternData) {
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
}

void PianoRollModule::onReset() {
  transport.reset();
  patternData.reset();
	processDivider.setDivision(32);
}

json_t *PianoRollModule::dataToJson() {
  json_t *rootJ = Module::dataToJson();
  if (rootJ == NULL) {
      rootJ = json_object();
  }

  json_object_set_new(rootJ, "patterns", patternData.dataToJson());
  json_object_set_new(rootJ, "currentPattern", json_integer(transport.currentPattern()));
  json_object_set_new(rootJ, "currentStep", json_integer(transport.currentStepInPattern()));
  json_object_set_new(rootJ, "clockDelay", json_integer(clockDelay));
  json_object_set_new(rootJ, "sequenceRunning", json_boolean(transport.isRunning()));

  return rootJ;
}

void PianoRollModule::dataFromJson(json_t *rootJ) {
  Module::dataFromJson(rootJ);

  json_t *clockDelayJ = json_object_get(rootJ, "clockDelay");
  if (clockDelayJ) {
    clockDelay = json_integer_value(clockDelayJ);
  }

  json_t *patternsJ = json_object_get(rootJ, "patterns");
  if (patternsJ) {
    patternData.dataFromJson(patternsJ);
  }

  json_t *currentPatternJ = json_object_get(rootJ, "currentPattern");
  if (currentPatternJ) {
    transport.setPattern(json_integer_value(currentPatternJ));
  }

  json_t *currentStepJ = json_object_get(rootJ, "currentStep");
  if (currentStepJ) {
    transport.setStepInPattern(json_integer_value(currentStepJ));
  }

  json_t *sequenceRunningJ = json_object_get(rootJ, "sequenceRunning");
  if (sequenceRunningJ) {
    transport.setRun(json_boolean_value(sequenceRunningJ));
  }
}

int quantizePitch(float voct) {
	int oct = floor(voct);
	int note = abs(static_cast<int>( roundf( ( voct * 12.0f) ) ) ) % 12;
	if (voct < 0.0f && note > 0) {
		note = 12 - note;
	}

	return ((oct + 4) * 12) + note;
}

void PianoRollModule::onAdd() {
	patternData.moduleId = this->id;
}

void PianoRollModule::process(const ProcessArgs &args) {
	if (!processDivider.process()) return;

	bool clockTick = false;

	while((int)clockBuffer.size() <= clockDelay) {
		clockBuffer.push(inputs[CLOCK_INPUT].getVoltage());
	}

	float currentClockLevel = 0.f;

	while((int)clockBuffer.size() > clockDelay) {
		currentClockLevel = clockBuffer.shift();
		clockTick |= clockInputTrigger.process(currentClockLevel);
	}

	if (resetInputTrigger.process(inputs[RESET_INPUT].getVoltage())) {
    transport.reset();
		gateOutputPulse.reset();
		if (currentClockLevel > 1.f) {
			clockTick = true;
		}
	}

	if (inputs[PATTERN_INPUT].getChannels() > 0) {
		int nextPattern = clamp(quantizePitch(inputs[PATTERN_INPUT].getVoltage())  - 48, 0, 63);
    transport.setPattern(nextPattern);
	}

	if (recordingIn.process(inputs[RECORD_INPUT].getVoltage())) {
    transport.toggleRecording();
	}

	if (runInputTrigger.process(inputs[RUN_INPUT].getVoltage())) {
		transport.toggleRun();

		if (currentClockLevel > 1.f && transport.currentStepInPattern() == -1) {
			clockTick = true;
		}

		if (!transport.isRunning()) {
			gateOutputPulse.reset();
		}
	}

	if (clockTick) {
    transport.advanceStep();
	}

	runInputActive.process(inputs[RUN_INPUT].getChannels() > 0);

	if (runInputActive.changed && transport.isRunning()) {
		if (runInputActive.value == true) {
			bool triggerGateAgain = gateOutputPulse.process(0);
			gateOutputPulse.reset();
			if (triggerGateAgain) {
				// We've plugged in, the sequence is running and our gate is high
				// Trigger the gate for the full plugged in duration (forever)
				gateOutputPulse.trigger(PLUGGED_GATE_DURATION);
			}
		}

		if (runInputActive.value == false) {
			float gateTimeRemaining = gateOutputPulse.remaining;
			bool triggerGateAgain = gateOutputPulse.process(0) && gateTimeRemaining > 0;
			gateOutputPulse.reset();
			if (triggerGateAgain) {
				// We've unplugged and the sequence is running and the gate is high
				// retrigger it for the time remaining if it had been triggered
				// when the cable was already unplugged. This is to prevent the gate sounding
				// forever - even when the clock is stopped
				gateOutputPulse.trigger(gateTimeRemaining);
			}
		}
	}

	if (!transport.isRecording()) {
    voctInBuffer.clear();
    gateInBuffer.clear();
    retriggerInBuffer.clear();
    velocityInBuffer.clear();
	}

	if (transport.isRecording() && transport.isRunning()) {

		while (!voctInBuffer.full()) { voctInBuffer.push(inputs[VOCT_INPUT].getVoltage()); }
		while (!gateInBuffer.full()) { gateInBuffer.push(inputs[GATE_INPUT].getVoltage()); }
		while (!retriggerInBuffer.full()) { retriggerInBuffer.push(inputs[RETRIGGER_INPUT].getVoltage()); }
		while (!velocityInBuffer.full()) { velocityInBuffer.push(inputs[VELOCITY_INPUT].getVoltage()); }

    int pattern = transport.currentPattern();
		int measure = transport.currentMeasure();
		int stepInMeasure = transport.currentStepInMeasure();

		if (inputs[VOCT_INPUT].getChannels() > 0) {
			auto voctIn = voctInBuffer.shift();
			patternData.setStepPitch(pattern, measure, stepInMeasure, quantizePitch(voctIn));
		}

		if (inputs[GATE_INPUT].getChannels() > 0) {
			auto gateIn = gateInBuffer.shift();

			if (clockTick && gateIn < 0.1f) {
        // Only turn off at the start of the step, user may let go early - we still want this step active
				patternData.setStepActive(pattern, measure, stepInMeasure, false);
			}
			
			if (gateIn >= 1.f) {
				patternData.setStepActive(pattern, measure, stepInMeasure, true);
			}
		}

		if (inputs[RETRIGGER_INPUT].getChannels() > 0) {
			auto retriggerIn = retriggerInBuffer.shift();

			if (clockTick && retriggerIn < 0.1f) {
        // Only turn off at the start of the step, this will only trigger briefly within the step
        patternData.setStepRetrigger(pattern, measure, stepInMeasure, false);
			}
			
			if (retriggerIn >= 1.f) {
        patternData.setStepRetrigger(pattern, measure, stepInMeasure, true);
			}
		}

		if (inputs[VELOCITY_INPUT].getChannels() > 0) {
			auto velocityIn = velocityInBuffer.shift();

			if (clockTick) {
        patternData.setStepVelocity(pattern, measure, stepInMeasure, 0.f);
			}
			
			if (velocityIn > 0.f) {
        patternData.increaseStepVelocityTo(pattern, measure, stepInMeasure, rescale(velocityIn, 0.f, 10.f, 0.f, 1.f));
			}
		}

	}

	if (auditioner.isAuditioning()) {
    int pattern = transport.currentPattern();
		int measure = auditioner.stepToAudition() / patternData.getStepsPerMeasure(pattern);
		int stepInMeasure = auditioner.stepToAudition() % patternData.getStepsPerMeasure(pattern);

		if (patternData.isStepActive(pattern, measure, stepInMeasure)) {
			bool retrigger = auditioner.consumeRetrigger();

			if (retrigger) {
				retriggerOutputPulse.trigger(1e-3f);
			}

			gateOutputPulse.trigger(AUDITION_GATE_DURATION);

			outputs[VELOCITY_OUTPUT].setChannels(1);
			outputs[VELOCITY_OUTPUT].setVoltage(patternData.getStepVelocity(pattern, measure, stepInMeasure) * 10.f);

			float octave = patternData.getStepPitch(pattern, measure, stepInMeasure) / 12;
			float semitone = patternData.getStepPitch(pattern, measure, stepInMeasure) % 12;

			outputs[VELOCITY_OUTPUT].setChannels(1);
			outputs[VOCT_OUTPUT].setVoltage((octave-4.f) + ((1.f/12.f) * semitone));
		}
	}

	if (auditioner.consumeStopEvent()) {
		gateOutputPulse.reset();
	}

	if (((transport.isRunning() && clockTick)) && !transport.isRecording()) {
		if (transport.isLastStepOfPattern()) {
			eopOutputPulse.trigger(1e-3f);
		}

    int pattern = transport.currentPattern();
		int measure = transport.currentMeasure();
		int stepInMeasure = transport.currentStepInMeasure();

		if (patternData.isStepActive(pattern, measure, stepInMeasure)) {
			if (gateOutputPulse.process(0) == false || patternData.isStepRetriggered(pattern, measure, stepInMeasure)) {
				retriggerOutputPulse.trigger(1e-3f);
			}

			gateOutputPulse.trigger(runInputActive.value ? PLUGGED_GATE_DURATION : UNPLUGGED_GATE_DURATION);

			outputs[VELOCITY_OUTPUT].setChannels(1);
			outputs[VELOCITY_OUTPUT].setVoltage(patternData.getStepVelocity(pattern, measure, stepInMeasure) * 10.f);

			float octave = patternData.getStepPitch(pattern, measure, stepInMeasure) / 12;
			float semitone = patternData.getStepPitch(pattern, measure, stepInMeasure) % 12;

			outputs[VOCT_OUTPUT].setChannels(1);
			outputs[VOCT_OUTPUT].setVoltage((octave-4.f) + ((1.f/12.f) * semitone));

		} else {
			gateOutputPulse.reset();
		}
	}

	outputs[RETRIGGER_OUTPUT].setChannels(1);
	outputs[RETRIGGER_OUTPUT].setVoltage(retriggerOutputPulse.process(args.sampleTime * processDivider.getDivision()) ? 10.f : 0.f);
	outputs[GATE_OUTPUT].setChannels(1);
	outputs[GATE_OUTPUT].setVoltage(gateOutputPulse.process(args.sampleTime * processDivider.getDivision()) ? 10.f : 0.f);
	if (outputs[RETRIGGER_OUTPUT].getChannels() == 0 && outputs[RETRIGGER_OUTPUT].getVoltage() > 0.f) {
		// If we're not using the retrigger output, the gate output to 0 for the trigger duration instead
		outputs[GATE_OUTPUT].setVoltage(0.f);
	}
	outputs[END_OF_PATTERN_OUTPUT].setChannels(1);
	outputs[END_OF_PATTERN_OUTPUT].setVoltage(eopOutputPulse.process(args.sampleTime * processDivider.getDivision()) ? 10.f : 0.f);

	if (inputs[GATE_INPUT].getChannels() > 0 && inputs[GATE_INPUT].getVoltage() > 1.f) {
		if (inputs[VOCT_INPUT].getChannels() > 0) { outputs[VOCT_OUTPUT].setVoltage(inputs[VOCT_INPUT].getVoltage()); }
		if (inputs[GATE_INPUT].getChannels() > 0) { 
			if (outputs[RETRIGGER_OUTPUT].getChannels() == 0 && inputs[RETRIGGER_INPUT].getChannels() > 0) {
				outputs[GATE_OUTPUT].setVoltage(inputs[GATE_INPUT].getVoltage() - inputs[RETRIGGER_INPUT].getVoltage());
			} else {
				outputs[GATE_OUTPUT].setVoltage(inputs[GATE_INPUT].getVoltage());
			}
		}
		if (inputs[RETRIGGER_INPUT].getChannels() > 0) { outputs[RETRIGGER_OUTPUT].setVoltage(inputs[RETRIGGER_INPUT].getVoltage()); }
		if (inputs[VELOCITY_INPUT].getChannels() > 0) { outputs[VELOCITY_OUTPUT].setVoltage(inputs[VELOCITY_INPUT].getVoltage()); }
	}

  // Send our chaining outputs
	outputs[CLOCK_OUTPUT].setChannels(1);
	outputs[RESET_OUTPUT].setChannels(1);
	outputs[PATTERN_OUTPUT].setChannels(1);
	outputs[RUN_OUTPUT].setChannels(1);
	outputs[RECORD_OUTPUT].setChannels(1);

	outputs[CLOCK_OUTPUT].setVoltage(inputs[CLOCK_INPUT].getVoltage());
	outputs[RESET_OUTPUT].setVoltage(inputs[RESET_INPUT].getVoltage());
	outputs[PATTERN_OUTPUT].setVoltage(transport.currentPattern() * (1.f/12.f));
	outputs[RUN_OUTPUT].setVoltage(inputs[RUN_INPUT].getVoltage());
	outputs[RECORD_OUTPUT].setVoltage(inputs[RECORD_INPUT].getVoltage());
}

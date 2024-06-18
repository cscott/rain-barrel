#ifndef NOBOUNCE_H
#define NOBOUNCE_H

#define NOBOUNCE_VERSION "0.0.1"

#include "AsyncDelay.h"

/*
 * An anti-bounce timer.
 */
#include <Arduino.h>

template <typename T> class NoBounce {
 public:

  inline NoBounce(T initial_state, unsigned long d, AsyncDelay::units_t u)
    : delay(AsyncDelay(d, u)), last_state(initial_state), next_state(initial_state), saw_flip(false) {
    delay.restart(); // don't change for <delay> after restart
  }
  
  T update(T new_state) {
    if (last_state != new_state) {
      if (delay.isExpired()) {
	// we want to flip and it's been long enough. do it!
	last_state = next_state = new_state;
	delay.restart();
      } else {
	// we want to flip but it hasn't been long enough
	next_state = new_state;
      }
    } else if (next_state != new_state) {
      // we want to stay as we are, but someone wanted something else
      next_state = new_state;
      delay.restart(); // reset the delay
    }
    return last_state;
  }

  T getState() {
    if (last_state != next_state && delay.isExpired()) {
      last_state = next_state;
      delay.restart();
    }
    return last_state;
  }

 private:
  T last_state, next_state;
  AsyncDelay delay;
  bool saw_flip;
};

#endif /* defined(NOBOUNCE_H) */

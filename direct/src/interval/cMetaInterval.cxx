// Filename: cMetaInterval.cxx
// Created by:  drose (27Aug02)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://www.panda3d.org/license.txt .
//
// To contact the maintainers of this program write to
// panda3d@yahoogroups.com .
//
////////////////////////////////////////////////////////////////////

#include "cMetaInterval.h"
#include "config_interval.h"
#include "indirectLess.h"
#include "indent.h"

#include <algorithm>
#include <math.h>   // for log10()
#include <stdio.h>  // for sprintf()

TypeHandle CMetaInterval::_type_handle;

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::Constructor
//       Access: Published
//  Description: 
////////////////////////////////////////////////////////////////////
CMetaInterval::
CMetaInterval(const string &name) :
  CInterval(name, 0.0, true)
{
  _precision = interval_precision;
  _current_nesting_level = 0;
  _next_event_index = 0;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::Destructor
//       Access: Published, Virtual
//  Description: 
////////////////////////////////////////////////////////////////////
CMetaInterval::
~CMetaInterval() {
  clear_intervals();
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::clear_intervals
//       Access: Published
//  Description: Resets the list of intervals and prepares for
//               receiving a new list.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
clear_intervals() {
  // Better not do this unless you have serviced all of the
  // outstanding events!
  nassertv(_event_queue.empty());

  clear_events();

  // Go through all of our nested intervals and remove ourselves as
  // their parent.
  Defs::iterator di;
  for (di = _defs.begin(); di != _defs.end(); ++di) {
    IntervalDef &def = (*di);
    if (def._c_interval != (CInterval *)NULL) {
      CInterval::Parents::iterator pi = 
        find(def._c_interval->_parents.begin(),
             def._c_interval->_parents.end(),
             this);
      nassertv(pi != def._c_interval->_parents.end());
      def._c_interval->_parents.erase(pi);
    }
  }
  _defs.clear();

  _current_nesting_level = 0;
  _next_event_index = 0;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::push_level
//       Access: Published
//  Description: Marks the beginning of a nested level of child
//               intervals.  Within the nested level, a RelativeStart
//               time of RS_level_begin refers to the start of the
//               level, and the first interval added within the level
//               is always relative to the start of the level.
//
//               The return value is the index of the def entry
//               created by this push.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
push_level(double rel_time, RelativeStart rel_to) {
  _defs.push_back(IntervalDef());
  IntervalDef &def = _defs.back();
  def._type = DT_push_level;
  def._rel_time = rel_time;
  def._rel_to = rel_to;
  _current_nesting_level++;
  mark_dirty();

  return (int)_defs.size() - 1;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::add_c_interval
//       Access: Published
//  Description: Adds a new CInterval to the list.  The interval will
//               be played when the indicated time (relative to the
//               given point) has been reached.
//
//               The return value is the index of the def entry
//               representing the new interval.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
add_c_interval(CInterval *c_interval, 
               double rel_time, RelativeStart rel_to) {
  nassertr(c_interval != (CInterval *)NULL, -1);

  c_interval->_parents.push_back(this);
  _defs.push_back(IntervalDef());
  IntervalDef &def = _defs.back();
  def._type = DT_c_interval;
  def._c_interval = c_interval;
  def._rel_time = rel_time;
  def._rel_to = rel_to;
  mark_dirty();

  return (int)_defs.size() - 1;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::add_ext_index
//       Access: Published
//  Description: Adds a new external interval to the list.  This
//               represents some object in the external scripting
//               language that has properties similar to a CInterval
//               (for instance, a Python Interval object).
//
//               The CMetaInterval object cannot play this external
//               interval directly, but it records a placeholder for
//               it and will ask the scripting language to play it
//               when it is time, via is_event_ready() and related
//               methods.
//
//               The ext_index number itself is simply a handle that
//               the scripting language makes up and associates with
//               its interval object somehow.  The CMetaInterval
//               object does not attempt to interpret this value.
//
//               The return value is the index of the def entry
//               representing the new interval.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
add_ext_index(int ext_index, const string &name, double duration,
              bool open_ended,
              double rel_time, RelativeStart rel_to) {
  _defs.push_back(IntervalDef());
  IntervalDef &def = _defs.back();
  def._type = DT_ext_index;
  def._ext_index = ext_index;
  def._ext_name = name;
  def._ext_duration = duration;
  def._ext_open_ended = open_ended;
  def._rel_time = rel_time;
  def._rel_to = rel_to;
  mark_dirty();

  return (int)_defs.size() - 1;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::pop_level
//       Access: Published
//  Description: Finishes a level marked by a previous call to
//               push_level(), and returns to the previous level.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
pop_level() {
  nassertr(_current_nesting_level > 0, -1);

  _defs.push_back(IntervalDef());
  IntervalDef &def = _defs.back();
  def._type = DT_pop_level;
  _current_nesting_level--;
  mark_dirty();

  return (int)_defs.size() - 1;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::initialize
//       Access: Published, Virtual
//  Description: This replaces the first call to step(), and indicates
//               that the interval has just begun.  This may be
//               overridden by derived classes that need to do some
//               explicit initialization on the first call.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
initialize(double t) {
  // It may be tempting to flush the event_queue here, but don't do
  // it.  Those are events that must still be serviced from some
  // previous interval operation.  Throwing them away would be a
  // mistake.

  recompute();
  _next_event_index = 0;
  _active.clear();

  int now = double_to_int_time(t);

  // Now look for events from the beginning up to the current time.
  ActiveEvents new_active;
  while (_next_event_index < _events.size() &&
         _events[_next_event_index]->_time <= now) {
    PlaybackEvent *event = _events[_next_event_index];
    
    // Do the indicated event.
    do_event_forward(event, new_active, true);
    _next_event_index++;
  }
  finish_events_forward(now, new_active);

  _curr_t = t;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::instant
//       Access: Published, Virtual
//  Description: This is called in lieu of initialize() .. step()
//               .. finalize(), when everything is to happen within
//               one frame.  The interval should initialize itself,
//               then leave itself in the final state.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
instant() {
  recompute();
  _active.clear();

  // Apply all of the events.  This just means we invoke "instant" for
  // any end or instant event, ignoring the begin events.
  PlaybackEvents::iterator ei;
  for (ei = _events.begin(); ei != _events.end(); ++ei) {
    PlaybackEvent *event = (*ei);
    if (event->_type != PET_begin) {
      enqueue_event(event->_n, ET_instant, true, 0);
    }
  }

  _next_event_index = _events.size();
  _curr_t = get_duration();
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::step
//       Access: Published, Virtual
//  Description: Advances the time on the interval.  The time may
//               either increase (the normal case) or decrease
//               (e.g. if the interval is being played by a slider).
////////////////////////////////////////////////////////////////////
void CMetaInterval::
step(double t) {
  int now = double_to_int_time(t);

  // Now look for events between the last time we ran and the current
  // time.

  if (_next_event_index < _events.size() &&
      _events[_next_event_index]->_time <= now) {
    // The normal case: time is increasing.
    ActiveEvents new_active;
    while (_next_event_index < _events.size() &&
           _events[_next_event_index]->_time <= now) {
      PlaybackEvent *event = _events[_next_event_index];

      // Do the indicated event.
      do_event_forward(event, new_active, false);
      _next_event_index++;
    }

    finish_events_forward(now, new_active);

  } else {
    // A less usual case: time is decreasing.
    ActiveEvents new_active;
    while (_next_event_index > 0 && 
           _events[_next_event_index - 1]->_time > now) {
      _next_event_index--;
      PlaybackEvent *event = _events[_next_event_index];
      do_event_reverse(event, new_active, false);
    }

    finish_events_reverse(now, new_active);
  }

  _curr_t = t;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::finalize
//       Access: Published, Virtual
//  Description: This is called when an interval is interrupted.  It
//               should advance the time as if step() were called, and
//               also perform whatever cleanup might be required.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
finalize() {
  // Do all remaining events.
  ActiveEvents new_active;
  while (_next_event_index < _events.size()) {
    PlaybackEvent *event = _events[_next_event_index];
    // Do the indicated event.
    do_event_forward(event, new_active, false);
    _next_event_index++;
  }

  _curr_t = get_duration();
  finish_events_forward(double_to_int_time(_curr_t), new_active);
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::reverse_initialize
//       Access: Published, Virtual
//  Description: Similar to initialize(), but this is called when the
//               interval is being played backwards; it indicates that
//               the interval should start at the finishing state and
//               undo any intervening intervals.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
reverse_initialize(double t) {
  // It may be tempting to flush the event_queue here, but don't do
  // it.  Those are events that must still be serviced from some
  // previous interval operation.  Throwing them away would be a
  // mistake.

  recompute();
  _next_event_index = _events.size();
  _active.clear();

  int now = double_to_int_time(t);

  // Now look for events from the end down to the current time.
  ActiveEvents new_active;
  while (_next_event_index > 0 && 
         _events[_next_event_index - 1]->_time > now) {
    _next_event_index--;
    PlaybackEvent *event = _events[_next_event_index];
    
    // Do the indicated event.
    do_event_reverse(event, new_active, true);
  }
  finish_events_reverse(now, new_active);

  _curr_t = t;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::reverse_instant
//       Access: Published, Virtual
//  Description: This is called in lieu of reverse_initialize()
//               .. step() .. reverse_finalize(), when everything is
//               to happen within one frame.  The interval should
//               initialize itself, then leave itself in the initial
//               state.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
reverse_instant() {
  recompute();
  _active.clear();

  // Apply all of the events.  This just means we invoke "instant" for
  // any end or instant event, ignoring the begin events.
  PlaybackEvents::reverse_iterator ei;
  for (ei = _events.rbegin(); ei != _events.rend(); ++ei) {
    PlaybackEvent *event = (*ei);
    if (event->_type != PET_begin) {
      enqueue_event(event->_n, ET_reverse_instant, true, 0);
    }
  }

  _next_event_index = 0;
  _curr_t = 0.0;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::reverse_finalize
//       Access: Published, Virtual
//  Description: Called generally following a reverse_initialize(),
//               this indicates the interval should set itself to the
//               initial state.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
reverse_finalize() {
  // Do all remaining events at the beginning.
  ActiveEvents new_active;

  while (_next_event_index > 0) {
    _next_event_index--;
    PlaybackEvent *event = _events[_next_event_index];
    do_event_reverse(event, new_active, false);
  }

  finish_events_reverse(0, new_active);
  _curr_t = 0.0;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::write
//       Access: Published, Virtual
//  Description: 
////////////////////////////////////////////////////////////////////
void CMetaInterval::
write(ostream &out, int indent_level) const {
  recompute();

  // How many digits of precision should we output for time?
  int num_decimals = (int)ceil(log10(_precision));
  int total_digits = num_decimals + 4;
  static const int max_digits = 32;  // totally arbitrary
  nassertv(total_digits <= max_digits);
  char format_str[12];
  sprintf(format_str, "%%%d.%df", total_digits, num_decimals);

  indent(out, indent_level) << get_name() << ":\n";

  int extra_indent_level = 1;
  Defs::const_iterator di;
  for (di = _defs.begin(); di != _defs.end(); ++di) {
    const IntervalDef &def = (*di);
    char time_str[max_digits + 1];
    sprintf(time_str, format_str, int_to_double_time(def._actual_begin_time));
    indent(out, indent_level) << time_str;

    switch (def._type) {
    case DT_c_interval:
      indent(out, extra_indent_level)
        << *def._c_interval;
      if (!def._c_interval->get_open_ended()) {
        out << " (!oe)";
      }
      out << "\n";
      break;

    case DT_ext_index:
      indent(out, extra_indent_level)
        << "*" << def._ext_name;
      if (def._ext_duration != 0.0) {
        out << " dur " << def._ext_duration;
      }
      if (!def._ext_open_ended) {
        out << " (!oe)";
      }
      out<< "\n";
      break;

    case DT_push_level:
      indent(out, extra_indent_level)
        << "{\n";
      extra_indent_level += 2;
      break;

    case DT_pop_level:
      extra_indent_level -= 2;
      indent(out, extra_indent_level)
        << "}\n";
      break;
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::do_recompute
//       Access: Protected, Virtual
//  Description: Recomputes all of the events (and the duration)
//               according to the set of interval defs.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
do_recompute() {
  _dirty = false;
  clear_events();

  int n = recompute_level(0, 0, _end_time);

  if (n != (int)_defs.size()) {
    interval_cat.warning()
      << "CMetaInterval pushes don't match pops.\n";
  }

  // We do a stable_sort() to guarantee ordering of events that have
  // the same start time.  These must be invoked in the order in which
  // they appear.
  stable_sort(_events.begin(), _events.end(), IndirectLess<PlaybackEvent>());
  _duration = int_to_double_time(_end_time);
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::clear_events
//       Access: Private
//  Description: Removes all entries from the _events list.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
clear_events() {
  PlaybackEvents::iterator ei;
  for (ei = _events.begin(); ei != _events.end(); ++ei) {
    PlaybackEvent *event = (*ei);
    delete event;
  }
  _events.clear();
  _active.clear();
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::do_event_forward
//       Access: Private
//  Description: Process a single event in the interval, moving
//               forwards in time.  If the event represents a new
//               begin, adds it to the new_active list; if it is an
//               end, finalizes it.
//
//               If is_initial is true, it is as if we are in
//               initialize: instant events will be invoked only if
//               they are marked open_ended.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
do_event_forward(CMetaInterval::PlaybackEvent *event, 
                 CMetaInterval::ActiveEvents &new_active, bool is_initial) {
  switch (event->_type) {
  case PET_begin:
    {
      nassertv(event->_begin_event == event);
      nassertv(_active.find(event) == _active.end());
      bool okflag = new_active.insert(event).second;
      nassertv(okflag);
    }
    break;
    
  case PET_end:
    {
      // Erase the event from either the new active or the current
      // active lists.
      int erase_count = new_active.erase(event->_begin_event);
      if (erase_count != 0) {
        // This interval was new this frame; we must invoke it as
        // an instant event.
        enqueue_event(event->_n, ET_instant, is_initial);
      } else {
        erase_count = _active.erase(event->_begin_event);
        enqueue_event(event->_n, ET_finalize, is_initial);
      }
      nassertv(erase_count == 1);
    }
    break;
    
  case PET_instant:
    nassertv(event->_begin_event == event);
    nassertv(new_active.find(event) == new_active.end());
    nassertv(_active.find(event) == _active.end());
    enqueue_event(event->_n, ET_instant, is_initial);
    break;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::finish_events_forward
//       Access: Private
//  Description: After walking through the event list and adding a
//               bunch of new events to new_active, finished up by
//               calling step() on all of the events still in _active
//               and initialize() on all the events in new_active,
//               then copying the events from new_active to active.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
finish_events_forward(int now, CMetaInterval::ActiveEvents &new_active) {
  // Do whatever's still active.
  ActiveEvents::iterator ai;
  for (ai = _active.begin(); ai != _active.end(); ++ai) {
    PlaybackEvent *event = (*ai);
    enqueue_event(event->_n, ET_step, false, now - event->_time);
  }
  
  // Initialize whatever new intervals we came across.
  for (ai = new_active.begin(); ai != new_active.end(); ++ai) {
    PlaybackEvent *event = (*ai);
    enqueue_event(event->_n, ET_initialize, false, now - event->_time);
    bool inserted = _active.insert(event).second;
    nassertv(inserted);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::do_event_reverse
//       Access: Private
//  Description: Process a single event in the interval, moving
//               backwards in time.  This undoes the indicated event.
//               If the event represents a new begin, adds it to the
//               new_active list; if it is an end, finalizes it.
//
//               If is_initial is true, it is as if we are in
//               reverse_initialize: instant events will be invoked
//               only if they are marked open_ended.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
do_event_reverse(CMetaInterval::PlaybackEvent *event, 
                 CMetaInterval::ActiveEvents &new_active, bool is_initial) {
  // Undo the indicated event.
  switch (event->_type) {
  case PET_begin:
    {
      nassertv(event->_begin_event == event);
      // Erase the event from either the new active or the current
      // active lists.
      int erase_count = new_active.erase(event);
      if (erase_count != 0) {
        // This interval was new this frame; we invoke it as an
        // instant event.
        enqueue_event(event->_n, ET_reverse_instant, is_initial);
      } else {
        erase_count = _active.erase(event->_begin_event);
        enqueue_event(event->_n, ET_reverse_finalize, is_initial);
      }
      nassertv(erase_count == 1);
    }
    break;
    
  case PET_end:
    {
      nassertv(new_active.find(event->_begin_event) == new_active.end());
      bool okflag = new_active.insert(event->_begin_event).second;
      nassertv(okflag);
    }
    break;
    
  case PET_instant:
    nassertv(event->_begin_event == event);
    nassertv(_active.find(event) == _active.end());
    nassertv(new_active.find(event) == new_active.end());
    enqueue_event(event->_n, ET_reverse_instant, is_initial);
    break;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::finish_events_reverse
//       Access: Private
//  Description: After walking through the event list and adding a
//               bunch of new events to new_active, finishes up by
//               calling step() on all of the events still in _active
//               and reverse_initialize() on all the events in
//               new_active, then copying the events from new_active
//               to active.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
finish_events_reverse(int now, CMetaInterval::ActiveEvents &new_active) {
  // Do whatever's still active.
  ActiveEvents::iterator ai;
  for (ai = _active.begin(); ai != _active.end(); ++ai) {
    PlaybackEvent *event = (*ai);
    enqueue_event(event->_n, ET_step, false, now - event->_time);
  }
  
  // Initialize whatever new intervals we came across.
  for (ai = new_active.begin(); ai != new_active.end(); ++ai) {
    PlaybackEvent *event = (*ai);
    enqueue_event(event->_n, ET_reverse_initialize, false, now - event->_time);
    bool inserted = _active.insert(event).second;
    nassertv(inserted);
  }
}
  
////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::enqueue_event
//       Access: Private
//  Description: Enqueues the indicated interval for invocation after
//               we have finished scanning for events that need
//               processing this frame.
//
//               is_initial is only relevant for event types
//               ET_instant or ET_reverse_instant, and indicates
//               whether we are in the initialize() (or
//               reverse_initialize()) call, and should therefore only
//               invoke open-ended intervals.
//
//               time is only relevant for ET_initialize,
//               ET_reverse_initialize, and ET_step.
////////////////////////////////////////////////////////////////////
void CMetaInterval::
enqueue_event(int n, CInterval::EventType event_type, bool is_initial, int time) {
  nassertv(n >= 0 && n < (int)_defs.size());
  const IntervalDef &def = _defs[n];
  switch (def._type) {
  case DT_c_interval:
    if (is_initial &&
        (event_type == ET_instant || event_type == ET_reverse_instant) &&
        !def._c_interval->get_open_ended()) {
      // Ignore a non-open-ended interval that we skipped completely
      // past on initialize().
      return;
    } else {
      if (_event_queue.empty()) {
        // if the event queue is empty, we can process this C++
        // interval immediately.  We only need to defer it if there
        // are external (e.g. Python) intervals in the queue that need
        // to be processed first.
        def._c_interval->set_t(int_to_double_time(time), event_type);
        return;
      }
    }
    break;

  case DT_ext_index:
    if (is_initial &&
        (event_type == ET_instant || event_type == ET_reverse_instant) &&
        !def._ext_open_ended) {
      // Ignore a non-open-ended interval that we skipped completely
      // past on initialize().
      return;
    }
    break;

  default:
    nassertv(false);
    return;
  }

  _event_queue.push_back(EventQueueEntry(n, event_type, time));
}
  
////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::service_event_queue
//       Access: Private
//  Description: Invokes whatever C++ intervals might be at the head
//               of the queue, and prepares for passing an external
//               interval to the scripting language.
//
//               The return value is true if there remains at least
//               one external event to be serviced, false if all
//               events are handled.
////////////////////////////////////////////////////////////////////
bool CMetaInterval::
service_event_queue() {
  while (!_event_queue.empty()) {
    const EventQueueEntry &entry = _event_queue.front();
    nassertr(entry._n >= 0 && entry._n < (int)_defs.size(), false);
    const IntervalDef &def = _defs[entry._n];
    switch (def._type) {
    case DT_c_interval:
      // Handle the C++ event.
      def._c_interval->set_t(int_to_double_time(entry._time), entry._event_type);
      break;

    case DT_ext_index:
      // Here's an external event; leave it there and return.
      return true;

    default:
      nassertr(false, false);
      return false;
    }
    _event_queue.pop_front();
  }

  // No more events on the queue.
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::recompute_level
//       Access: Private
//  Description: Recursively recomputes a complete level (delimited by
//               push/pop definitions).
//
//               The value n on entry refers to the first entry after
//               the push; the return value will reference the
//               matching pop, or an index greater than the last
//               element in the array if there was no matching pop.
//
//               The level_begin value indicates the begin time of
//               this level.  On return, level_end is filled with the
//               end time of this level.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
recompute_level(int n, int level_begin, int &level_end) {
  level_end = level_begin;
  int previous_begin = level_begin;
  int previous_end = level_begin;

  while (n < (int)_defs.size() && _defs[n]._type != DT_pop_level) {
    IntervalDef &def = _defs[n];
    int begin_time, end_time;
    switch (def._type) {
    case DT_c_interval:
      begin_time = get_begin_time(def, level_begin, previous_begin, previous_end);
      def._actual_begin_time = begin_time;
      end_time = begin_time + double_to_int_time(def._c_interval->get_duration());
      if (begin_time == end_time) {
        _events.push_back(new PlaybackEvent(begin_time, n, PET_instant));
      } else {
        PlaybackEvent *begin = new PlaybackEvent(begin_time, n, PET_begin);
        PlaybackEvent *end = new PlaybackEvent(end_time, n, PET_end);
        end->_begin_event = begin;
        _events.push_back(begin);
        _events.push_back(end);
      }
      break;

    case DT_ext_index:
      begin_time = get_begin_time(def, level_begin, previous_begin, previous_end);
      def._actual_begin_time = begin_time;
      end_time = begin_time + double_to_int_time(def._ext_duration);
      if (begin_time == end_time) {
        _events.push_back(new PlaybackEvent(begin_time, n, PET_instant));
      } else {
        PlaybackEvent *begin = new PlaybackEvent(begin_time, n, PET_begin);
        PlaybackEvent *end = new PlaybackEvent(end_time, n, PET_end);
        end->_begin_event = begin;
        _events.push_back(begin);
        _events.push_back(end);
      }
      break;

    case DT_push_level:
      begin_time = get_begin_time(def, level_begin, previous_begin, previous_end);
      def._actual_begin_time = begin_time;
      n = recompute_level(n + 1, begin_time, end_time);
      break;

    case DT_pop_level:
      nassertr(false, _defs.size());
      break;
    }

    previous_begin = begin_time;
    previous_end = end_time;
    level_end = max(level_end, end_time);
    n++;
  }

  if (n < (int)_defs.size()) {
    // The final pop "begins" at the level end time, just for clarity
    // on output.
    IntervalDef &def = _defs[n];
    def._actual_begin_time = level_end;
  }

  return n;
}

////////////////////////////////////////////////////////////////////
//     Function: CMetaInterval::get_begin_time
//       Access: Private
//  Description: Returns the integer begin time indicated by the given
//               IntervalDef, given the indicated level begin,
//               previous begin, and previous end times.
////////////////////////////////////////////////////////////////////
int CMetaInterval::
get_begin_time(const CMetaInterval::IntervalDef &def, int level_begin,
               int previous_begin, int previous_end) {
  switch (def._rel_to) {
  case RS_previous_end:
    return previous_end + double_to_int_time(def._rel_time);

  case RS_previous_begin:
    return previous_begin + double_to_int_time(def._rel_time);

  case RS_level_begin:
    return level_begin + double_to_int_time(def._rel_time);
  }

  nassertr(false, previous_end);
  return previous_end;
}

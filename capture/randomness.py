# SPDX-License-Identifier: AGPL-3.0-or-later
"""Random decision capture and strict correctness replay (not a speed benchmark).

Capture delegates exactly once and never changes normal results. Replay accepts
reordered populations, not missing/extra work. Both streams share one Tape.
"""
from .schema import number
try:
    from _mlat_capture_native import random_keys as _native_keys
except ImportError:
    _native_keys = None

LIMIT = 4096
STREAMS = frozenset(('tracker', 'coordinator'))


def item_key(item, receiver_identity):
    if type(item) is tuple:
        if len(item) != 4: raise ValueError('rate pair shape')
        product, receiver, aircraft, rate = item
        key = ('pair', receiver_identity(receiver), aircraft.icao, number(product), number(rate))
    else:
        key = ('aircraft', item.icao)
    validate_key(key)
    return key


def validate_key(key):
    if type(key) not in (tuple, list) or not key:
        raise ValueError('choice item shape')
    if key[0] == 'aircraft' and len(key) == 2:
        address = key[1]
    elif key[0] == 'pair' and len(key) == 5:
        if type(key[1]) is not int or not 1 <= key[1] <= 65536:
            raise ValueError('choice receiver bound')
        address = key[2]
        number(key[3]); number(key[4])
    else:
        raise ValueError('unknown choice item')
    if type(address) is not int or not 0 <= address <= 0xffffff:
        raise ValueError('choice aircraft bound')
    return tuple(key)


def validate(value, known=None):
    if type(value) is not dict or value.get('stream') not in STREAMS:
        raise ValueError('choice stream')
    op = value.get('op')
    if op == 'uniform':
        if value.keys() != {'stream', 'op', 'value'} or not 0 <= number(value['value']) < 1:
            raise ValueError('uniform choice bounds')
        return value
    if op not in ('sample', 'shuffle') or value.keys() != {'stream', 'op', 'population', 'selected'}:
        raise ValueError('choice fields')
    keys = []
    for name in ('population', 'selected'):
        items = value[name]
        if type(items) not in (list, tuple) or len(items) > LIMIT:
            raise ValueError('choice population bound')
        items = [validate_key(key) for key in items]
        if len(set(items)) != len(items): raise ValueError('duplicate choice identity')
        if known is not None and any(k[0] == 'pair' and k[1] not in known for k in items):
            raise ValueError('unknown choice receiver')
        keys.append(set(items))
    if not keys[1] <= keys[0] or (op == 'shuffle' and keys[0] != keys[1]):
        raise ValueError('choice selection mismatch')
    return value


class RecordingRandom:
    def __init__(self, delegate, recorder, receiver_identity, stream):
        if stream not in STREAMS: raise ValueError('choice stream')
        self.delegate, self.recorder, self.identity, self.stream = delegate, recorder, receiver_identity, stream

    def _keys(self, items):
        if len(items) > LIMIT: raise ValueError('choice population bound')
        if _native_keys is not None and type(items) in (list, tuple):
            return _native_keys(items, self.identity)
        return tuple(item_key(x, self.identity) for x in items)

    def _fail(self):
        try: self.recorder.invalidate("random_schema")
        except Exception: pass

    def _record(self, op, **fields):
        if not self.recorder.active: return
        try:
            value = validate(dict(stream=self.stream, op=op, **fields))
            self.recorder.submit('random_choice', 0, value)
        except Exception:
            self._fail()

    def _record_checked_keys(self, op, population, selected):
        """Keys came from _keys (bounds and scalar validation already done).

        Still check duplicates and selection integrity. Decoder validate() is
        deliberately unchanged; only repeated producer scalar checks disappear.
        """
        if not self.recorder.active: return
        try:
            if getattr(self.recorder, '_native_random', None) is not None:
                self.recorder.record_random_choice(self.stream, op, population, selected)
                return
            available, chosen = set(population), set(selected)
            if (len(available) != len(population) or len(chosen) != len(selected)
                    or not chosen <= available or (op == 'shuffle' and chosen != available)):
                raise ValueError('choice selection mismatch')
            self.recorder.submit('random_choice', 0, dict(stream=self.stream, op=op,
                population=population, selected=selected))
        except Exception:
            self._fail()

    def random(self):
        result = self.delegate.random()
        self._record('uniform', value=result)
        return result

    def sample(self, population, k):
        result = self.delegate.sample(population, k=k)
        if self.recorder.active:
            try: self._record_checked_keys('sample', self._keys(population), self._keys(result))
            except Exception: self._fail()
        return result

    def shuffle(self, population):
        active = self.recorder.active
        keys = None
        if active:
            try: keys = self._keys(population)
            except Exception: pass  # still execute the original shuffle exactly once
        result = self.delegate.shuffle(population)
        if active:
            try:
                if keys is None: raise ValueError('unrecordable population')
                self._record_checked_keys('shuffle', keys, self._keys(population))
            except Exception: self._fail()
        return result


class Tape:
    def __init__(self, records):
        self.records = iter(records)
        self.used = 0
        self.failed = False
        self.finished = False

    def next(self, stream, op):
        try:
            if self.failed or self.finished: raise ValueError('random tape invalid or finished')
            try: value = next(self.records)
            except StopIteration: raise ValueError('random tape exhausted') from None
            validate(value)
            if value['stream'] != stream or value['op'] != op: raise ValueError('random call order changed')
            self.used += 1
            return value
        except BaseException:
            self.failed = True
            raise

    def finish(self):
        if self.failed: raise ValueError('random tape invalid')
        if self.finished: return
        try:
            try: next(self.records)
            except StopIteration:
                self.finished = True
                return
            raise ValueError('unused random choices')
        except BaseException:
            self.failed = True
            raise


class ReplayRandom:
    def __init__(self, tape, receiver_identity, stream):
        if stream not in STREAMS: raise ValueError('choice stream')
        self.tape, self.identity, self.stream = tape, receiver_identity, stream

    def random(self): return self.tape.next(self.stream, 'uniform')['value']

    def _select(self, population, op, k):
        try:
            if len(population) > LIMIT: raise ValueError('choice population bound')
            value = self.tape.next(self.stream, op)
            mapping = {item_key(item, self.identity): item for item in population}
            if len(mapping) != len(population) or set(mapping) != {tuple(x) for x in value['population']}:
                raise ValueError('replay population changed')
            if type(k) is not int or k != len(value['selected']): raise ValueError('replay selection size changed')
            return [mapping[tuple(key)] for key in value['selected']]
        except BaseException:
            self.tape.failed = True
            raise

    def sample(self, population, k): return self._select(population, 'sample', k)

    def shuffle(self, population):
        try: population[:] = self._select(population, 'shuffle', len(population))
        except BaseException:
            self.tape.failed = True
            raise

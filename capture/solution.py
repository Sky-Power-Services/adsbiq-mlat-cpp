# SPDX-License-Identifier: AGPL-3.0-or-later
"""Accepted output solution after altitude replacement/UKF update, not raw LM."""
from .schema import number

FIELDS = frozenset(('time', 'address', 'ecef', 'cov', 'cluster', 'broadcast',
                    'distinct', 'dof', 'error', 'filter'))
FILTER_FIELDS = frozenset(('valid', 'last_update', 'mean', 'cov'))


def vector(value, size):
    if len(value) != size:
        raise ValueError('solution vector shape')
    return tuple(number(float(x)) for x in value)


def matrix(value, size):
    if value is None:
        return None
    if len(value) != size:
        raise ValueError('solution matrix shape')
    return tuple(vector(row, size) for row in value)


def make(timestamp, address, ecef, covariance, cluster, broadcast, distinct, dof, kalman, error, identity):
    if len(cluster) > 4096 or len(broadcast) > 4096:
        raise ValueError('solution receiver bound')
    value = dict(time=number(timestamp), address=address, ecef=vector(ecef, 3), cov=matrix(covariance, 3),
        cluster=tuple(identity(r) for r in cluster), broadcast=tuple(identity(r) for r in broadcast),
        distinct=distinct, dof=dof, error=None if error is None else number(float(error)),
        filter=dict(valid=bool(kalman.valid), last_update=number(kalman.last_update),
                    mean=None if kalman._mean is None else vector(kalman._mean, 9), cov=matrix(kalman._cov, 9)))
    validate(value, set(value['cluster']) | set(value['broadcast']))
    return value


def validate(value, known):
    if type(value) is not dict or value.keys() != FIELDS:
        raise ValueError('solution fields')
    number(value['time'])
    for key, low, high in (('address', 0, 0xffffff), ('distinct', 0, 4096), ('dof', 0, 4096)):
        if type(value[key]) is not int or not low <= value[key] <= high:
            raise ValueError('solution integer bound')
    # Decoder validation is exact numeric types (no coerced untrusted strings).
    def checked_vector(v, n):
        if type(v) not in (list, tuple) or len(v) != n:
            raise ValueError('solution vector shape')
        for x in v: number(x)
    def checked_matrix(v, n):
        if v is not None:
            if type(v) not in (list, tuple) or len(v) != n: raise ValueError('solution matrix shape')
            for row in v: checked_vector(row, n)
    checked_vector(value['ecef'], 3)
    checked_matrix(value['cov'], 3)
    if value['error'] is not None: number(value['error'])
    for key in ('cluster', 'broadcast'):
        ids = value[key]
        if type(ids) not in (tuple, list) or len(ids) > 4096:
            raise ValueError('solution receiver bound')
        if any(type(i) is not int or i not in known for i in ids) or len(set(ids)) != len(ids):
            raise ValueError('unknown or duplicate solution receiver')
    f = value['filter']
    if type(f) is not dict or f.keys() != FILTER_FIELDS or type(f['valid']) is not bool:
        raise ValueError('solution filter fields')
    number(f['last_update'])
    if f['mean'] is not None: checked_vector(f['mean'], 9)
    checked_matrix(f['cov'], 9)
    if (f['mean'] is None) != (f['cov'] is None) or (f['valid'] and f['mean'] is None):
        raise ValueError('incomplete filter state')

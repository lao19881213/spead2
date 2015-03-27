"""Test that data can be passed over the SPEAD protocol, using the various
transports, and mixing with the old PySPEAD implementation.
"""

from __future__ import division, print_function
import numpy as np
from contextlib import contextmanager
import itertools
import spead2
import spead2.send
import spead2.recv
from decorator import decorator
from nose.tools import *
from nose.plugins.skip import SkipTest
try:
    import spead64_40
except ImportError:
    spead64_40 = None
try:
    import spead64_48
except ImportError:
    spead64_48 = None


def assert_items_equal(item1, item2):
    assert_equal(item1.id, item2.id)
    assert_equal(item1.name, item2.name)
    assert_equal(item1.description, item2.description)
    assert_equal(item1.shape, item2.shape)
    assert_equal(item1.format, item2.format)
    # Byte order need not match, provided that values are received correctly
    if item1.dtype is not None and item2.dtype is not None:
        assert_equal(item1.dtype.newbyteorder('<'), item2.dtype.newbyteorder('<'))
    else:
        assert_equal(item1.dtype, item2.dtype)
    assert_equal(item1.order, item2.order)
    # Comparing arrays has many issues. Convert them to lists where appropriate
    value1 = item1.value
    value2 = item2.value
    if hasattr(value1, 'tolist'):
        value1 = value1.tolist()
    if hasattr(value2, 'tolist'):
        value2 = value2.tolist()
    assert_equal(value1, value2)

def assert_item_groups_equal(item_group1, item_group2):
    assert_equal(sorted(item_group1.keys()), sorted(item_group2.keys()))
    for key in item_group1.keys():
        assert_items_equal(item_group1[key], item_group2[key])

@decorator
def no_legacy_send(test, *args, **kwargs):
    if not args[0].is_legacy_send:
        test(*args, **kwargs)

@decorator
def no_legacy_receive(test, *args, **kwargs):
    if not args[0].is_legacy_receive:
        test(*args, **kwargs)

@decorator
def no_legacy(test, *args, **kwargs):
    if not (args[0].is_legacy_send or args[0].is_legacy_receive):
        test(*args, **kwargs)

class BaseTestPassthrough(object):
    """Tests common to all transports and libraries"""

    is_legacy_send = False
    is_legacy_receive = False

    def _test_item_group(self, item_group):
        received_item_group = self.transmit_item_group(item_group)
        assert_item_groups_equal(item_group, received_item_group)

    def test_numpy_simple(self):
        """A basic array with numpy encoding"""
        ig = spead2.send.ItemGroup()
        data = np.array([[6, 7, 8], [10, 11, 12000]], dtype=np.uint16)
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=data.shape, dtype=data.dtype, value=data)
        self._test_item_group(ig)

    def test_numpy_large(self):
        """A numpy style array split across several packets"""
        ig = spead2.send.ItemGroup()
        data = np.random.randn(100, 200)
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=data.shape, dtype=data.dtype, value=data)

    def test_fallback_struct_whole_bytes(self):
        """A structure with non-byte-aligned elements, but which is
        byte-aligned overall."""
        ig = spead2.send.ItemGroup()
        format = [('u', 4), ('f', 64), ('i', 4)]
        data = (12, 1.5, -3)
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=(), dtype=None, format=format, value=data)
        self._test_item_group(ig)

    @no_legacy_receive
    def test_fallback_array_partial_bytes_small(self):
        """An array which takes a fractional number of bytes per element
        and is small enough to encode in an immediate.

        It is disabled for PySPEAD receive because PySPEAD does not decode
        such items in the same way as it encodes them.
        """
        ig = spead2.send.ItemGroup()
        format = [('u', 7)]
        data = [127, 12, 123]
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=(len(data),), dtype=None, format=format, value=data)
        self._test_item_group(ig)

    @no_legacy
    def test_fallback_types(self):
        """An array structure using a mix of types."""
        ig = spead2.send.ItemGroup()
        format = [('b', 1), ('c', 7), ('f', 32)]
        data = [(True, 'y', 1.0), (False, 'n', -1.0)]
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=(2,), dtype=None, format=format, value=data)
        self._test_item_group(ig)

    @no_legacy
    def test_numpy_fallback_struct(self):
        """A structure specified using a format, but which is encodable using
        numpy."""
        ig = spead2.send.ItemGroup()
        format = [('u', 8), ('f', 32)]
        data = (12, 1.5)
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=(), dtype=None, format=format, value=data)
        self._test_item_group(ig)

    @no_legacy
    def test_fallback_struct_partial_bytes(self):
        """A structure which takes a fractional number of bytes per element.
        """
        ig = spead2.send.ItemGroup()
        format = [('u', 4), ('f', 64)]
        data = (12, 1.5)
        ig.add_item(id=0x2345, name='name', description='description',
                    shape=(), dtype=None, format=format, value=data)
        self._test_item_group(ig)

    def transmit_item_group(self, item_group):
        """Transmit `item_group` over the chosen transport, and return the
        item group received at the other end.
        """
        raise NotImplementedError()


class TestPassthroughUdp(BaseTestPassthrough):
    def transmit_item_group(self, item_group):
        thread_pool = spead2.ThreadPool(2)
        sender = spead2.send.UdpStream(
                thread_pool, "localhost", 8888,
                spead2.send.StreamConfig(rate=1e8),
                buffer_size=0)
        receiver = spead2.recv.Stream(thread_pool)
        receiver.add_udp_reader(8888, bind_hostname="localhost")
        gen = spead2.send.HeapGenerator(item_group)
        sender.send_heap(gen.get_heap())
        sender.send_heap(gen.get_end())
        received_item_group = spead2.ItemGroup()
        for heap in receiver:
            received_item_group.update(heap)
        return received_item_group


class TestPassthroughMem(BaseTestPassthrough):
    def transmit_item_group(self, item_group):
        thread_pool = spead2.ThreadPool(2)
        sender = spead2.send.BytesStream(thread_pool)
        gen = spead2.send.HeapGenerator(item_group)
        sender.send_heap(gen.get_heap())
        sender.send_heap(gen.get_end())
        receiver = spead2.recv.Stream(thread_pool)
        receiver.add_buffer_reader(sender.getvalue())
        received_item_group = spead2.ItemGroup()
        for heap in receiver:
            received_item_group.update(heap)
        return received_item_group


class BaseTestPassthroughLegacySend(BaseTestPassthrough):
    is_legacy_send = True

    def transmit_item_group(self, item_group):
        if not self.spead:
            raise SkipTest('spead module not importable')
        thread_pool = spead2.ThreadPool(1)
        sender = self.spead.Transmitter(self.spead.TransportUDPtx('127.0.0.1', 8888, rate=1e9))
        receiver = spead2.recv.Stream(thread_pool, bug_compat=spead2.BUG_COMPAT_PYSPEAD_0_5_2)
        receiver.add_udp_reader(8888, bind_hostname='localhost')
        legacy_item_group = self.spead.ItemGroup()
        for item in item_group.values():
            legacy_item_group.add_item(
                    id=item.id,
                    name=item.name,
                    description=item.description,
                    shape=item.shape,
                    fmt=self.spead.mkfmt(*item.format) if item.format else self.spead.DEFAULT_FMT,
                    ndarray=item.value if isinstance(item.value, np.ndarray) else None)
            legacy_item_group[item.name] = item.value
        sender.send_heap(legacy_item_group.get_heap())
        sender.end()
        received_item_group = spead2.ItemGroup()
        for heap in receiver:
            received_item_group.update(heap)
        return received_item_group


class TestPassthroughLegacySend64_40(BaseTestPassthroughLegacySend):
    spead = spead64_40


class TestPassthroughLegacySend64_48(BaseTestPassthroughLegacySend):
    spead = spead64_48


class BaseTestPassthroughLegacyReceive(BaseTestPassthrough):
    is_legacy_receive = True

    @contextmanager
    def get_receiver(self, *args, **kwargs):
        receiver = self.spead.TransportUDPrx(*args, **kwargs)
        yield receiver
        receiver.stop()

    def transmit_item_group(self, item_group):
        if not self.spead:
            raise SkipTest('spead module not importable')
        thread_pool = spead2.ThreadPool(1)
        with self.get_receiver(8888) as receiver:
            sender = spead2.send.UdpStream(
                    thread_pool, "localhost", 8888,
                    spead2.send.StreamConfig(rate=1e7),
                    buffer_size=0)
            gen = spead2.send.HeapGenerator(
                    item_group,
                    heap_address_bits=self.heap_address_bits,
                    bug_compat=spead2.BUG_COMPAT_PYSPEAD_0_5_2)
            sender.send_heap(gen.get_heap())
            # We can't send an end-of-stream, because PySPEAD stops
            # processing packets as soon as an end-of-stream packet
            # arrives, even if there are still queued packets.
            legacy_item_group = self.spead.ItemGroup()
            for heap in itertools.islice(self.spead.iterheaps(receiver), 1):
                legacy_item_group.update(heap)
            received_item_group = spead2.ItemGroup()
            for key in legacy_item_group.keys():
                item = legacy_item_group.get_item(key)
                if item.dtype is None:
                    received_item_group.add_item(
                            id=item.id,
                            name=item.name,
                            description=item.description,
                            shape=item.shape,
                            dtype=None,
                            format=list(self.spead.parsefmt(item.format)),
                            value=item.get_value())
                else:
                    received_item_group.add_item(
                            id=item.id,
                            name=item.name,
                            description=item.description,
                            shape=item.shape,
                            dtype=item.dtype,
                            order='F' if item.fortran_order else 'C',
                            value=item.get_value())
            return received_item_group


class TestPassthroughLegacyReceive64_40(BaseTestPassthroughLegacyReceive):
    spead = spead64_40
    heap_address_bits = 40


class TestPassthroughLegacyReceive64_48(BaseTestPassthroughLegacyReceive):
    spead = spead64_48
    heap_address_bits = 48
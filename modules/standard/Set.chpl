/*
 * Copyright 2020-2025 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
  This module contains the implementation of Chapel's standard 'set' type.

  A set is a collection of unique elements. Sets are unordered and unindexed.

  The highly parallel nature of Chapel means that great care should be taken
  when performing operations that may invalidate references to set elements.
  Adding or removing an element from a set may invalidate references to
  elements contained in the set.

  All references to set elements are invalidated when the set is cleared or
  deinitialized.

  Sets are not parallel safe by default, but can be made parallel safe by
  setting the param formal 'parSafe` to true in any set constructor. When
  constructed from another set, the new set will inherit the parallel safety
  mode of its originating set.

  When using set operators (e.g., ``A | B``), if both sets contain elements that
  are ``==`` equivalent, the element from the first argument  of the operation
  will always be chosen for the resultant set. This may happen if the ``==``
  operator has been overloaded on the set element type, causing values of
  the set type to be ``==`` equivalent, even when there may be differences
  between the elements of the first argument and the elements of the second
  argument.
*/
module Set {

  //
  // Use this to restrict our secondary initializer to only resolve when the
  // "iterable" argument has a method named "these".
  //
  import ChapelLocks;
  private use IO;
  private use Reflection;
  private use ChapelHashtable;
  private use HaltWrappers;

  @chpldoc.nodoc
  private param _sanityChecks = true;

  //
  // Some asserts are useful while developing, but can be turned off when the
  // implementation is correct.
  //
  private inline proc _sanity(expr: bool) {
    if _sanityChecks then
      assert(expr);
  }

  //
  // We can change the lock type later. Use a spinlock for now, even if it
  // is suboptimal in cases where long critical sections have high
  // contention.
  //
  @chpldoc.nodoc
  type _lockType = ChapelLocks.chpl_LocalSpinlock;

  //
  // Use a wrapper class to let set methods have a const ref receiver even
  // when `parSafe` is `true` and the set lock is used.
  //
  @chpldoc.nodoc
  class _LockWrapper {
    var lockVar = new _lockType();

    inline proc lock() {
      lockVar.lock();
    }

    inline proc unlock() {
      lockVar.unlock();
    }
  }

  @chpldoc.nodoc
  proc _checkElementType(type t) {
    // In the future we might support it if the set is not default-inited.
    if isGenericType(t) {
      compilerWarning('creating a set with element type ' + t:string, 2);
      compilerError('set element type cannot currently be generic', 2);
    }
  }

  // If we have "chpl__serialize", assume we have "chpl__deserialize".
  @chpldoc.nodoc
  proc _isSerializable(type T) param {
    use Reflection;
    pragma "no init"
    var x: T;
    return canResolveMethod(x, "chpl__serialize");
  }

  /* Impacts whether the copy initializer that takes a set will generate a
     warning when the other set has a different ``parSafe`` setting than the
     destination.  Compile with ``-swarnForSetParsafeMismatch=false`` to turn
     off this warning.

     Defaults to ``true``
  */
  config param warnForSetParsafeMismatch = true;


  /*
    A set is a collection of unique elements. Attempting to add a duplicate
    element to a set has no effect.

    The set type supports a test for membership via the :proc:`contains`
    method, along with free functions for calculating the union, difference,
    intersection, and symmetric difference of two sets. The set type also
    defines the (proper) subset and (proper) superset operations by
    overloading common comparison operators.

    Sets can be iterated over. The set type makes no guarantee of a consistent
    iteration order.

    A set can be default initialized (containing no elements), or it may be
    initialized with elements that are copies of those contained by any
    type that supports an iterator.

    The set type is not parallel safe by default. For situations in which
    such protections are desirable, parallel safety can be enabled by setting
    `parSafe = true` in any set constructor. A set constructed from another
    set inherits the parallel safety mode of that set by default. Note that
    the ``parSafe`` mode is currently unstable and will eventually be replaced
    by a standalone parallel-safe set type.
  */
  record set : serializable {

    /* The type of the elements contained in this set. */
    type eltType;

    // NOTE: the compiler has some special handling for unstable warnings
    // associated with set's parSafe field:
    // * AggregateType::generateType -> ensures that specifying 'parSafe' in a type
    //    expression for 'set' will generate a warning
    // * functionResolution.createGenericRecordVarDefaultInitCall -> ensures that
    //    the stable initializer is called when the compiler generates initializer
    //     calls for variable declarations that don't specify 'parSafe' (or set it to false)
    //
    // This results in the following behavior:
    //  - 'var m: set(int)' doesn't generate an unstable warning
    //  - 'type t = set(int, false)' generates an unstable warning
    //  - 'var m: set(int, parSafe=true)' generates two unstable warnings (one
    //    for the type expression and one for the initializer call)
    //  - 'var m: set(int, parSafe=false)' generates one unstable warning for
    //    the type expression

    /* If `true`, this set will perform parallel safe operations. */
    @unstable("'set.parSafe' is unstable and is expected to be replaced by a separate set type in the future")
    param parSafe = false;

    /*
       Fractional value that specifies how full this map can be
       before requesting additional memory. The default value of
       0.5 means that the map will not resize until the map is more
       than 50% full. The acceptable values for this argument are
       between 0 and 1, exclusive, meaning (0,1). This is useful
       when you would like to reduce memory impact or potentially
       speed up how fast the map finds a slot. To override the
       default value of 0.5, the `defaultHashTableResizeThreshold`
       config flag can be set at runtime. Note that this default
       affects all hash-based data structures, including
       associative domains and maps.
    */
    const resizeThreshold = defaultHashTableResizeThreshold;

    @chpldoc.nodoc
    var _lock = if parSafe then new _LockWrapper() else none;

    @chpldoc.nodoc
    var _htb: chpl__hashtable(eltType, nothing);

    /*
      Initializes an empty set containing elements of the given type.

      :arg eltType: The type of the elements of this set.
      :arg resizeThreshold: Fractional value that specifies how full this map
                            can be before requesting additional memory.
      :arg initialCapacity: Integer value that specifies starting map size. The
                            map can hold at least this many values before
                            attempting to resize.
    */
    proc init(type eltType,
              resizeThreshold=defaultHashTableResizeThreshold,
              initialCapacity=16) {
      _checkElementType(eltType);
      this.eltType = eltType;
      this.parSafe = false;
      if resizeThreshold <= 0 || resizeThreshold >= 1 {
        warning("'resizeThreshold' must be between 0 and 1.",
                        " 'resizeThreshold' will be set to 0.5");
        this.resizeThreshold = 0.5;
      } else {
        this.resizeThreshold = resizeThreshold;
      }
      this._htb = new chpl__hashtable(eltType, nothing, this.resizeThreshold,
                                      initialCapacity);
    }

    @unstable("'set.parSafe' is unstable and is expected to be replaced by a separate set type in the future")
    proc init(type eltType, param parSafe,
              resizeThreshold=defaultHashTableResizeThreshold,
              initialCapacity=16) {
      _checkElementType(eltType);
      this.eltType = eltType;
      this.parSafe = parSafe;
      if resizeThreshold <= 0 || resizeThreshold >= 1 {
        warning("'resizeThreshold' must be between 0 and 1.",
                        " 'resizeThreshold' will be set to 0.5");
        this.resizeThreshold = 0.5;
      } else {
        this.resizeThreshold = resizeThreshold;
      }
      this._htb = new chpl__hashtable(eltType, nothing, this.resizeThreshold,
                                      initialCapacity);
    }

    /*
      Initialize this set with a unique copy of each element contained in
      `iterable`. If an element from `iterable` is already contained in this
      set, it will not be added again. The formal `iterable` must be a type
      with an iterator named "these" defined for it.

      :arg eltType: The type of the elements of this set.
      :arg iterable: A collection of elements to add to this set.
      :arg resizeThreshold: Fractional value that specifies how full this map
                            can be before requesting additional memory.
      :arg initialCapacity: Integer value that specifies starting map size. The
                            map can hold at least this many values before
                            attempting to resize.
    */
    proc init(type eltType, iterable,
              resizeThreshold=defaultHashTableResizeThreshold,
              initialCapacity=16)
    where canResolveMethod(iterable, "these") lifetime this < iterable {
      _checkElementType(eltType);

      this.eltType = eltType;
      this.parSafe = false;
      if resizeThreshold <= 0 || resizeThreshold >= 1 {
        warning("'resizeThreshold' must be between 0 and 1.",
                        " 'resizeThreshold' will be set to 0.5");
        this.resizeThreshold = 0.5;
      } else {
        this.resizeThreshold = resizeThreshold;
      }
      this._htb = new chpl__hashtable(eltType, nothing, this.resizeThreshold,
                                      initialCapacity);
      init this;

      for elem in iterable do _addElem(elem);
    }

    @unstable("'set.parSafe' is unstable and is expected to be replaced by a separate set type in the future")
    proc init(type eltType, iterable, param parSafe,
              resizeThreshold=defaultHashTableResizeThreshold,
              initialCapacity=16)
    where canResolveMethod(iterable, "these") lifetime this < iterable {
      _checkElementType(eltType);

      this.eltType = eltType;
      this.parSafe = parSafe;
      if resizeThreshold <= 0 || resizeThreshold >= 1 {
        warning("'resizeThreshold' must be between 0 and 1.",
                        " 'resizeThreshold' will be set to 0.5");
        this.resizeThreshold = 0.5;
      } else {
        this.resizeThreshold = resizeThreshold;
      }
      this._htb = new chpl__hashtable(eltType, nothing, this.resizeThreshold,
                                      initialCapacity);
      init this;

      for elem in iterable do _addElem(elem);
    }

    /*
      Initialize this set with a copy of each of the elements contained in
      the set `other`. This set will inherit the `parSafe` value of the
      set `other`.

      :arg other: A set to initialize this set with.
    */
    proc init=(const ref other: set(?t, ?p)) lifetime this < other {
      this.eltType = if this.type.eltType != ? then
                        this.type.eltType else t;
      this.parSafe = if this.type.parSafe != ? then
                        this.type.parSafe else p;
      this.resizeThreshold = other.resizeThreshold;
      this._htb = new chpl__hashtable(eltType, nothing,
                                      resizeThreshold);

      if (this.parSafe != other.parSafe && warnForSetParsafeMismatch) {
        compilerWarning("initializing between two sets with different " +
                        "parSafe settings\n" + "Note: this warning can be " +
                        "silenced with '-swarnForSetParsafeMismatch=false'");
      }

      init this;

      // TODO: Relax this to allow if 'isCoercible(t, this.eltType)'?
      if eltType != t {
        compilerError('cannot initialize ', this.type:string, ' from ',
                      other.type:string, ' due to element type ',
                      'mismatch');
      } else if !isCopyableType(eltType) {
        compilerError('cannot initialize ', this.type:string, ' from ',
                      other.type:string, ' because element type ',
                      eltType:string, ' is not copyable');
      } else {
        // TODO: Use a forall when this.parSafe?
        for elem in other do _addElem(elem);
      }
    }

    // Do things the slow/copy way if the element is serializable.
    // See issue: #17477
    @chpldoc.nodoc
    proc ref _addElem(in elem: eltType): bool
    where _isSerializable(eltType) {
        var result = false;

        on this {
          var (isFullSlot, idx) = _htb.findAvailableSlot(elem);

          if !isFullSlot {
            _htb.fillSlot(idx, elem, none);
            result = true;
          }
        }

      return result;
    }

    // For types that aren't serializable, avoid an extra copy by moving
    // the value across locales.
    @chpldoc.nodoc
    proc ref _addElem(pragma "no auto destroy" in elem: eltType): bool {
      use MemMove;

      var result = false;

      on this {

        // TODO: The following variation gets lifetime errors in
        // '.../Set/types/testNilableTuple.chpl':
        //
        // var moved = moveFrom(elem);
        // var (isFullSlot, idx) = _htb.findAvailableSlot(moved);
        //
        var (isFullSlot, idx) = _htb.findAvailableSlot(elem);

        if !isFullSlot {

          // This line moves the bits over, 'elem' is dead past this point.
          var moved = moveFrom(elem);
          _htb.fillSlot(idx, moved, none);
          result = true;
        } else {

          // The set contains the value of 'elem', so clean 'elem' up.
          chpl__autoDestroy(elem);
        }
      }

      return result;
    }

    @chpldoc.nodoc
    inline proc _enter() {
      if parSafe then
        on this {
          _lock.lock();
        }
    }

    @chpldoc.nodoc
    inline proc _leave() {
      if parSafe then
        on this {
          _lock.unlock();
        }
    }

    /*
      Add a copy of the element `element` to this set. Does nothing if this set
      already contains an element equal to the value of `element`.

      :arg element: The element to add to this set.
    */
    proc ref add(in element: eltType) lifetime this < element {

      // Remove `on this` block because it prevents copy elision of `element`
      // when passed to `_addElem`. See #15808.
      _enter(); defer _leave();
      _addElem(element);
    }

    /*
      Returns `true` if the given element is a member of this set, and `false`
      otherwise.

      :arg element: The element to test for membership.
      :return: Whether or not the given element is a member of this set.
      :rtype: `bool`
    */
    proc const contains(const ref element: eltType): bool {
      var result = false;

      on this {
        _enter(); defer _leave();
        result = _contains(element);
      }

      return result;
    }

    /*
     As above, but parSafe lock must be held and must be called "on this".
    */
    @chpldoc.nodoc
    proc const _contains(const ref element: eltType): bool {
      var (hasFoundSlot, _) = _htb.findFullSlot(element);
      return hasFoundSlot;
    }

    /*
      Returns `true` if this set shares no elements in common with the set
      `other`, and `false` otherwise.

      .. warning::

        `other` must not be modified during this call.

      :arg other: The set to compare against.
      :return: Whether or not this set and `other` are disjoint.
      :rtype: `bool`
    */
    proc const isDisjoint(const ref other: set(eltType, ?)): bool {
      var result = true;

      on this {
        _enter(); defer _leave();

        if _size != 0 {
          // TODO: Take locks on other?
          for x in other do
            if this._contains(x) {
              result = false;
              break;
            }
        }
      }

      return result;
    }

    /*
      Attempt to remove the item from this set with a value equal to `element`.
      If an element equal to `element` was removed from this set, return `true`,
      else return `false` if no such value was found.

      .. warning::

        Removing an element from this set may invalidate existing references
        to the elements contained in this set.

      :arg element: The element to remove.
      :return: Whether or not an element equal to `element` was removed.
      :rtype: `bool`
    */
    proc ref remove(const ref element: eltType): bool {
      var result = false;

      on this {
        _enter(); defer _leave();

        var (hasFoundSlot, idx) = _htb.findFullSlot(element);

        if hasFoundSlot {
          // TODO: Return the removed element? #15819
          var key: eltType;
          var val: nothing;

          _htb.clearSlot(idx, key, val);
          _htb.maybeShrinkAfterRemove();
          result = true;
        }
      }

      return result;
    }

    /*
      Clear the contents of this set.

      .. warning::

        Clearing the contents of this set will invalidate all existing
        references to the elements contained in this set.
    */
    proc ref clear() {
      on this {
        _enter(); defer _leave();

        for idx in 0..#_htb.tableSize {
          if _htb.isSlotFull(idx) {
            var key: eltType;
            var val: nothing;
            _htb.clearSlot(idx, key, val);
          }
        }

        _htb.maybeShrinkAfterRemove();
      }
    }

    /*
      Iterate over the elements of this set. Yields constant references
      that cannot be modified.

      .. warning::

        Modifying this set while iterating over it may invalidate the
        references returned by an iterator and is considered undefined
        behavior.

      :yields: A constant reference to an element in this set.
    */
    iter const these() const ref {
      foreach idx in 0..#_htb.tableSize {
        if _htb.isSlotFull(idx) then yield _htb.table[idx].key;
      }
    }

    @chpldoc.nodoc
    iter const these(param tag) const ref where tag == iterKind.standalone {
      var space = 0..#_htb.tableSize;
      foreach idx in space.these(tag) do
        if _htb.isSlotFull(idx) then yield _htb.table[idx].key;
    }

    @chpldoc.nodoc
    iter const these(param tag) where tag == iterKind.leader {
      for followThis in _htb._evenSlots(tag) {
        yield followThis;
      }
    }

    @chpldoc.nodoc
    iter const these(param tag, followThis) const ref
    where tag == iterKind.follower {
      foreach val in _htb._evenSlots(followThis, tag) {
        yield val.key;
      }
    }

    @chpldoc.nodoc
    proc const _defaultWriteHelper(ch: fileWriter) throws {
      on this {
        _enter(); defer _leave();

        var count = 1;
        ch.write("{");

        for x in this {
          if count <= (_htb.tableNumFullSlots - 1) {
            count += 1;
            ch.write(x); ch.write(", ");
          } else {
            ch.write(x);
          }
        }

        ch.write("}");
      }
    }

    /*
      Write the contents of this set to a fileWriter.

      :arg writer: A fileWriter to write to.
      :arg serializer: The serializer to use when writing.
    */
    proc const serialize(writer:fileWriter(?), ref serializer) throws {
      if isDefaultSerializerType(serializer.type) {
        _defaultWriteHelper(writer);
      } else {
        on this {
          _enter(); defer _leave();
          var ser = serializer.startList(writer, this.size);
          for x in this do ser.writeElement(x);
          ser.endList();
        }
      }
    }

    @chpldoc.nodoc
    proc ref deserialize(reader, ref deserializer) throws {
      on this {
        _enter(); defer _leave();

        this.clear();

        if deserializer.type == IO.defaultDeserializer {
          reader.readLiteral("{");

          do {
            this.add(reader.read(eltType));
          } while reader.matchLiteral(",");

          reader.readLiteral("}");
        } else {
          var des = deserializer.startList(reader);
          while des.hasMore() do
            this.add(des.readElement(eltType));
          des.endList();
        }
      }
    }

    @chpldoc.nodoc
    proc init(type eltType, param parSafe : bool,
              reader, ref deserializer) throws {
      this.eltType = eltType;
      this.parSafe = parSafe;

      init this;

      this.deserialize(reader, deserializer);
    }

    /*
      Returns `true` if this set contains zero elements.

      :return: `true` if this set is empty.
      :rtype: `bool`
    */
    inline proc const isEmpty(): bool {
      var result = false;

      on this {
        _enter(); defer _leave();
        result = _htb.tableNumFullSlots == 0;
      }

      return result;
    }

    /*
      The current number of elements contained in this set.
    */
    inline proc const size {
      var result = 0;

      on this {
        _enter(); defer _leave();
        result = _size;
      }

      return result;
    }

    /*
      As above, but the parSafe lock must be held, and must be called
      "on this".
    */
    @chpldoc.nodoc
    inline proc const _size {
      return _htb.tableNumFullSlots;
    }

    /*
      Returns a new DefaultRectangular array containing a copy of each of the
      elements contained in this set. The elements of the returned array are
      not guaranteed to follow any particular ordering.

      :return: An array containing a copy of each of the elements in this set.
      :rtype: `[] eltType`
    */
    proc const toArray(): [] eltType {
      // May take locks non-locally...
      _enter(); defer _leave();

      var result: [0..#_htb.tableNumFullSlots] eltType;

      if !isCopyableType(eltType) then
        compilerError('Cannot create array because set element type ' +
                      eltType:string + ' is not copyable');

      on this {
        if _htb.tableNumFullSlots != 0 {
          var count = 0;
          var array: [0..#_htb.tableNumFullSlots] eltType;

          for x in this {
            array[count] = x;
            count += 1;
          }

          result = array;
        }
      }

      return result;
    }

  } // End record "set".

  /*
    Clear the contents of the set `lhs`, then iterate through the contents of
    `rhs` and add a copy of each element to `lhs`.

    .. warning::

      This will invalidate any references to elements previously contained in
      the set `lhs`.

    :arg lhs: The set to assign to.
    :arg rhs: The set to assign from.
  */
  operator set.=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    lhs.clear();

    for x in rhs do
      lhs.add(x);
  }

  /*
    Return a new set that contains the union of two sets.

    :arg a: A set to take the union of.
    :arg b: A set to take the union of.

    :return: A new set containing the union between `a` and `b`.
  */
  operator set.|(const ref a: set(?t, ?), const ref b: set(t, ?)) {
    var result: set(t, (a.parSafe || b.parSafe));

    // TODO: Split-init causes weird errors, remove this line and then run
    // setCompositionParSafe.chpl to see.
    result;

    result = a;
    result |= b;

    return result;
  }

  /*
    Add to the set `lhs` all the elements of `rhs`.

    :arg lhs: A set to take the union of and then assign to.
    :arg rhs: A set to take the union of.
  */
  operator set.|=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    for x in rhs do
      lhs.add(x);
  }

  /*
    Return a new set that contains the union of two sets. Alias for the `|`
    operator.

    :arg a: A set to take the union of.
    :arg b: A set to take the union of.

    :return: A new set containing the union between `a` and `b`.
  */
  operator set.+(const ref a: set(?t, ?), const ref b: set(t, ?)) {
    return a | b;
  }

  /*
    Add to the set `lhs` all the elements of `rhs`.

    :arg lhs: A set to take the union of and then assign to.
    :arg rhs: A set to take the union of.
  */
  operator set.+=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    lhs |= rhs;
  }

  /*
    Return a new set that contains the difference of two sets.

    :arg a: A set to take the difference of.
    :arg b: A set to take the difference of.

    :return: A new set containing the difference between `a` and `b`.
  */
  operator set.-(const ref a: set(?t, ?), const ref b: set(t, ?)) {
    var result = new set(t, (a.parSafe || b.parSafe));

    if a.parSafe && b.parSafe {
      forall x in a with (ref result) do
        if !b.contains(x) then
          result.add(x);
    } else {
      for x in a do
        if !b.contains(x) then
          result.add(x);
    }

    return result;
  }

  /*
    Remove from the set `lhs` the elements of `rhs`.

    .. warning::

      This will invalidate any references to elements previously contained in
      the set `lhs`.

    :arg lhs: A set to take the difference of and then assign to.
    :arg rhs: A set to take the difference of.
  */
  operator set.-=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    if lhs.parSafe && rhs.parSafe {
      forall x in rhs with (ref lhs) do
        lhs.remove(x);
    } else {
      for x in rhs do
        lhs.remove(x);
    }
  }

  /*
    Return a new set that contains the intersection of two sets.

    :arg a: A set to take the intersection of.
    :arg b: A set to take the intersection of.

    :return: A new set containing the intersection of `a` and `b`.
  */
  operator set.&(const ref a: set(?t, ?), const ref b: set(t, ?)) {
    var result: set(t, (a.parSafe || b.parSafe));

    /* Iterate over the smaller set */
    if a.size <= b.size {
      if a.parSafe && b.parSafe {
        forall x in a with (ref result) do
          if b.contains(x) then
            result.add(x);
      } else {
        for x in a do
          if b.contains(x) then
            result.add(x);
      }
    } else {
      if a.parSafe && b.parSafe {
        forall x in b with (ref result) do
          if a.contains(x) then
            result.add(x);
      } else {
        for x in b do
          if a.contains(x) then
            result.add(x);
      }
    }

    return result;
  }

  /*
    Assign to the set `lhs` the set that is the intersection of `lhs` and
    `rhs`.

    .. warning::

      This will invalidate any references to elements previously contained in
      the set `lhs`.

    :arg lhs: A set to take the intersection of and then assign to.
    :arg rhs: A set to take the intersection of.
  */
  operator set.&=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    /* We can't remove things from lhs while iterating over it, so
     * use a temporary. */
    var result: set(t, (lhs.parSafe || rhs.parSafe));

    if lhs.parSafe && rhs.parSafe {
      forall x in lhs with (ref result) do
        if rhs.contains(x) then
          result.add(x);
    } else {
      for x in lhs do
        if rhs.contains(x) then
          result.add(x);
    }

    lhs = result;
  }

  /*
    Return the symmetric difference of two sets.

    :arg a: A set to take the symmetric difference of.
    :arg b: A set to take the symmetric difference of.

    :return: A new set containing the symmetric difference of `a` and `b`.
  */
  operator set.^(const ref a: set(?t, ?), const ref b: set(t, ?)) {
    var result: set(t, (a.parSafe || b.parSafe));

    // TODO: Split-init causes weird errors, remove this line and then run
    // setCompositionParSafe.chpl to see.
    result;

    /* Expect the loop in ^= to be more expensive than the loop in =,
       so arrange for the rhs of the ^= to be the smaller set. */
    if a.size <= b.size {
      result = b;
      result ^= a;
    } else {
      result = a;
      result ^= b;
    }

    return result;
  }

  /*
    Assign to the set `lhs` the set that is the symmetric difference of `lhs`
    and `rhs`.

    .. warning::

      This will invalidate any references to elements previously contained in
      the set `lhs`.

    :arg lhs: A set to take the symmetric difference of and then assign to.
    :arg rhs: A set to take the symmetric difference of.
  */
  operator set.^=(ref lhs: set(?t, ?), const ref rhs: set(t, ?)) {
    if lhs.parSafe && rhs.parSafe {
      forall x in rhs with (ref lhs) {
        if lhs.contains(x) {
          lhs.remove(x);
        } else {
          lhs.add(x);
        }
      }
    } else {
      for x in rhs {
        if lhs.contains(x) {
          lhs.remove(x);
        } else {
          lhs.add(x);
        }
      }
    }
  }

  /*
    Return `true` if the sets `a` and `b` are equal. That is, they are the
    same size and contain the same elements.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if two sets are equal.
    :rtype: `bool`
  */
  operator set.==(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    if a.size != b.size then
      return false;

    var result = true;

    if a.parSafe && b.parSafe {
      forall x in a do
        if !b.contains(x) then
          result = false;
    } else {
      for x in a do
        if !b.contains(x) then
          return false;
    }

    return result;
  }

  @chpldoc.nodoc
  operator set.:(x: set(?et1, ?p1), type t: set(?et2, ?p2)) {
    // TODO: Allow coercion between element types? If we do then init=
    // should also be changed accordingly.
    if et1 != et2 then
      compilerError('Cannot cast to set with different ',
                    'element type: ', t:string);
    var result: set(et1, p2) = x;
    return result;
  }

  /*
    Return `true` if the sets `a` and `b` are not equal.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if two sets are not equal.
    :rtype: `bool`
  */
  operator set.!=(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    return !(a == b);
  }

  /*
    Return `true` if `a` is a proper subset of `b`.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if `a` is a proper subset of `b`.
    :rtype: `bool`
  */
  operator set.<(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    if a.size >= b.size then
      return false;
    return a <= b;
  }

  /*
    Return `true` if `a` is a subset of `b`.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if `a` is a subset of `b`.
    :rtype: `bool`
  */
  operator set.<=(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    if a.size > b.size then
      return false;

    var result = true;

    // TODO: Do we need to guard/make result atomic here?
    if a.parSafe && b.parSafe {
      forall x in a do
        if !b.contains(x) then
          result = false;
    } else {
      for x in a do
        if !b.contains(x) then
          return false;
    }

    return result;
  }

  /*
    Return `true` if `a` is a proper superset of `b`.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if `a` is a proper superset of `b`.
    :rtype: `bool`
  */
  operator set.>(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    if a.size <= b.size then
      return false;
    return a >= b;
  }

  /*
    Return `true` if `a` is a superset of `b`.

    :arg a: A set to compare.
    :arg b: A set to compare.

    :return: `true` if `a` is a superset of `b`.
    :rtype: `bool`
  */
  operator set.>=(const ref a: set(?t, ?), const ref b: set(t, ?)): bool {
    if a.size < b.size then
      return false;

    var result = true;

    if a.parSafe && b.parSafe {
      forall x in b do
        if !a.contains(x) then
          result = false;
    } else {
      for x in b do
        if !a.contains(x) then
          return false;
    }

    return result;
  }

} // End module "Set".

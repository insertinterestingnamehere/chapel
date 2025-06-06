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

/* Utility routines for splitting a range into multiple chunks.

   The ``RangeChunk`` module assists with dividing a bounded ``range`` of any ``idxType``
   and stride into ``numChunks``. Chunks are 0-based, with the ``0`` index chunk including
   ``range.lowBound`` and the ``numChunks - 1`` index chunk including ``range.highBound``.

   Chunks are accessible in several ways:

     * as a range, through an iterator
     * as a range, through a query
     * as a tuple of 0-based orders into the range, through an iterator
     * as a tuple of 0-based orders into the range, through a query

   Given that it will be uncommon for the length of a given ``range`` to be divisible by
   ``numChunks``, there are three different remainder policies available, expressed
   by the enum ``RemElems``.
*/
module RangeChunk {
  private use Math;

  /*
     ``RemElems`` specifies the distribution of remainder elements.
  */
  enum RemElems {
    /*
      Default policy; remainder elements will be distributed throughout
      ``numChunks`` chunks
    */
    Thru,
    /*
      Chunks at the front will receive ``ceil(range.size / numChunks)``
      elements, then one chunk will receive what is left over; the actual number of chunks
      may be less than ``numChunks``
    */
    Pack,
    /*
      In ``numChunks`` chunks, every chunk that has an index less than
      ``range.size % numChunks`` will receive a remainder element
    */
    Mod
  }
  private use RemElems;

  /*
     Iterates through chunks ``0`` to ``numChunks - 1`` of range ``r``, emitting each
     as a range. The remainders will be distributed according to ``remPol``.
  */
  iter chunks(r: range(?), numChunks: integral, remPol: RemElems = Thru) {
    compilerAssert(r.bounds == boundKind.both,
                   "chunks() requires a bounded range, got ", r.type:string);
    foreach (startOrder, endOrder) in chunksOrder(r, numChunks, remPol) {
      const start = r.orderToIndex(startOrder);
      const end = r.orderToIndex(endOrder);
      yield ( start..end by r.stride ): r.type;
    }
  }

  /*
     Returns the ``idx`` chunk of range ``r`` as a range. The remainders will be
     distributed according to ``remPol``.
  */
  proc chunk(r: range(?),
             numChunks: integral, idx: integral, remPol: RemElems = Thru) {
    compilerAssert(r.bounds == boundKind.both,
                   "chunk() requires a bounded range, got ", r.type:string);
    const (startOrder, endOrder) = chunkOrder(r, numChunks, idx, remPol);
    const start = r.orderToIndex(startOrder);
    const end = r.orderToIndex(endOrder);
    return ( start..end by r.stride ): r.type;
  }

  /*
    Iterates through a range ``r``, which is blocked up in repeating ```nTasks```
    blocks of size ```blockSize```. Blocks are indexed from 0..nTasks-1 and the
    iterator emits all blocks with index ```tid``` in a cyclic manner.
  */
  //  Eg :
  //  1. For a range 1..15 and 4 blocks of size 2
  //  2. The block indexes range : 0-2
  //  3. The range is blocked up as following block indexes :
  //      1,2, 3,4, 5,6, 7,8, 9,10, 11,12, 13,14, 15
  //       0    1    2    3    0      1      2     3
  //  4. For a desired tid 2, the following chunks are emitted
  //      (5,6) (13,14)
  iter blockCyclicChunks(r: range(?),
                         blockSize: integral, tid: integral,
                         nTasks: integral) {
    compilerAssert(r.bounds == boundKind.both,
      "blockCyclicChunks() requires a bounded range, got ", r.type:string);
    if (tid >= nTasks) then
      halt("Parameter tid must be < nTasks " +
           "because blocks are indexed from 0..nTasks-1");

    if (blockSize <= 0) then
      halt("blockSize must a positive number");

    if (nTasks <= 0) then
      halt("nTasks must be a positive number");

    var rangeStride = r.stride;
    var blockStride = blockSize * rangeStride;
    var low = r.lowBound;
    var high = r.highBound;
    var firstBlockStart = (if r.hasPositiveStride() then r.lowBound  else r.highBound) +
                            blockStride * tid;
    if firstBlockStart > r.highBound || firstBlockStart < r.lowBound then return;

    var strideToNextBlock = blockStride * nTasks;

    if r.hasPositiveStride() {
      for blockStart in firstBlockStart..high by strideToNextBlock {
        var blockEnd = min(high, blockStart + blockStride - 1);
        yield ( blockStart..blockEnd by r.stride ): r.type;
      }
    } else {
      for blockEnd in low..firstBlockStart by strideToNextBlock {
        var blockStart = max(low, blockEnd + blockStride + 1);
        yield ( blockStart..blockEnd by r.stride ): r.type;
      }
    }
  }

  /*
     Iterates through chunks ``0`` to ``numChunks - 1`` of range ``r``, emitting each
     as a 0-based order tuple. The remainders will be distributed according to ``remPol``.
  */
  iter chunksOrder(r: range(?RT, boundKind.both, ?), numChunks: integral,
                   remPol: RemElems = Thru): 2*RT {
    if r.sizeAs(RT) == 0 || numChunks <= 0 then
      return;
    const nElems = r.sizeAs(RT);
    var nChunks = min(numChunks, nElems): RT;

    var chunkSize, rem: RT;
    select (remPol) {
      when Pack {
        chunkSize = nElems / nChunks;
        if chunkSize * nChunks != nElems {
          chunkSize += 1;
          nChunks = divCeil(nElems, chunkSize);
        }
      }
      when Mod {
        chunkSize = nElems / nChunks;
        rem = nElems - chunkSize * nChunks;
      }
    }

    foreach i in 0..<nChunks {
      var chunk: 2*RT;
      select (remPol) {
        when Thru do chunk = chunkOrderThru(nElems, nChunks, i);
        when Pack do chunk = chunkOrderPack(chunkSize, nElems, i);
        when Mod  do chunk = chunkOrderMod(chunkSize, rem, nElems, nChunks, i);
        otherwise halt("RangeChunk: unknown RemElems in chunksOrder");
      }
      yield chunk;
    }
  }

  /*
     Returns the ``idx`` chunk of range ``r`` as a 0-based order tuple. The remainders
     will be distributed according to ``remPol``.
  */
  proc chunkOrder(r: range(?RT, boundKind.both, ?),
                  numChunks: integral, idx: integral,
                  remPol: RemElems = Thru): 2*RT {
    if r.sizeAs(RT) == 0 || numChunks <= 0 || idx < 0 || idx >= numChunks then
      return (1: RT, 0: RT);

    const nElems = r.sizeAs(RT);
    const nChunks = min(numChunks, nElems): RT;
    const i = idx: RT;

    select (remPol) {
      when Thru {
        return chunkOrderThru(nElems, nChunks, i);
      }
      when Pack {
        var chunkSize = nElems / nChunks;
        if chunkSize * nChunks != nElems then
          chunkSize += 1;
        return chunkOrderPack(chunkSize, nElems, i);
      }
      when Mod {
        const chunkSize = nElems / nChunks;
        const rem = nElems - chunkSize * nChunks;
        return chunkOrderMod(chunkSize, rem, nElems, nChunks, i);
      }
      otherwise {
        halt("RangeChunk: unknown RemElems in chunkOrder");
      }
    }
  }


  //
  // Private helpers for order pairs and thereby ranges.
  // Each corresponds with a remainder policy.
  //
  @chpldoc.nodoc
  private proc chunkOrderThru(nElems: ?I, nChunks: I, i: I): (I, I) {
    const m = nElems * i;
    const start = if i == 0
      then 0: I
      else divCeil(m, nChunks);
    const end = if i == nChunks - 1
      then nElems - 1
      else divCeil(m + nElems, nChunks) - 1;
    return (start, end);
  }

  @chpldoc.nodoc
  private proc chunkOrderPack(chunkSize: ?I, nElems: I, i: I): (I, I) {
    const start = chunkSize * i;
    if start >= nElems then
      return (1: I, 0: I);

    var end = start + chunkSize - 1;
    if end >= nElems then
      end = nElems - 1;
    return (start, end);
  }

  @chpldoc.nodoc
  private proc chunkOrderMod(chunkSize: ?I, rem: I, nElems: I, nChunks: I,
                             i: I): (I, I) {
    var start, end: I;
    if i < rem {
      start = i * (chunkSize + 1);
      end = start + chunkSize;
    } else {
      start = nElems - (nChunks - i) * chunkSize;
      end = start + chunkSize - 1;
    }
    return (start, end);
  }
}

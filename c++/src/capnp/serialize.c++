// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "serialize.h"
#include "layout.h"
#include <kj/debug.h>
#include <exception>

namespace capnp {

FlatArrayMessageReader::FlatArrayMessageReader(
    kj::ArrayPtr<const word> array, ReaderOptions options)
    : MessageReader(options), end(array.end()) {
  if (array.size() < 1) {
    // Assume empty message.
    return;
  }

  const _::WireValue<uint32_t>* table =
      reinterpret_cast<const _::WireValue<uint32_t>*>(array.begin());

  uint segmentCount = table[0].get() + 1;
  size_t offset = segmentCount / 2u + 1u;

  KJ_REQUIRE(array.size() >= offset, "Message ends prematurely in segment table.") {
    return;
  }

  if (segmentCount == 0) {
    end = array.begin() + offset;
    return;
  }

  uint segmentSize = table[1].get();

  KJ_REQUIRE(array.size() >= offset + segmentSize,
             "Message ends prematurely in first segment.") {
    return;
  }

  segment0 = array.slice(offset, offset + segmentSize);
  offset += segmentSize;

  if (segmentCount > 1) {
    moreSegments = kj::heapArray<kj::ArrayPtr<const word>>(segmentCount - 1);

    for (uint i = 1; i < segmentCount; i++) {
      uint segmentSize = table[i + 1].get();

      KJ_REQUIRE(array.size() >= offset + segmentSize, "Message ends prematurely.") {
        moreSegments = nullptr;
        return;
      }

      moreSegments[i - 1] = array.slice(offset, offset + segmentSize);
      offset += segmentSize;
    }
  }

  end = array.begin() + offset;
}

kj::ArrayPtr<const word> FlatArrayMessageReader::getSegment(uint id) {
  if (id == 0) {
    return segment0;
  } else if (id <= moreSegments.size()) {
    return moreSegments[id - 1];
  } else {
    return nullptr;
  }
}

kj::Array<word> messageToFlatArray(kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  kj::Array<word> result = kj::heapArray<word>(computeSerializedSizeInWords(segments));

  _::WireValue<uint32_t>* table =
      reinterpret_cast<_::WireValue<uint32_t>*>(result.begin());

  // We write the segment count - 1 because this makes the first word zero for single-segment
  // messages, improving compression.  We don't bother doing this with segment sizes because
  // one-word segments are rare anyway.
  table[0].set(segments.size() - 1);

  for (uint i = 0; i < segments.size(); i++) {
    table[i + 1].set(segments[i].size());
  }

  if (segments.size() % 2 == 0) {
    // Set padding byte.
    table[segments.size() + 1].set(0);
  }

  word* dst = result.begin() + segments.size() / 2 + 1;

  for (auto& segment: segments) {
    memcpy(dst, segment.begin(), segment.size() * sizeof(word));
    dst += segment.size();
  }

  KJ_DASSERT(dst == result.end(), "Buffer overrun/underrun bug in code above.");

  return kj::mv(result);
}

size_t computeSerializedSizeInWords(kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  KJ_REQUIRE(segments.size() > 0, "Tried to serialize uninitialized message.");

  size_t totalSize = segments.size() / 2 + 1;

  for (auto& segment: segments) {
    totalSize += segment.size();
  }

  return totalSize;
}

// =======================================================================================

InputStreamMessageReader::InputStreamMessageReader(
    kj::InputStream& inputStream, ReaderOptions options, kj::ArrayPtr<word> scratchSpace)
    : MessageReader(options), inputStream(inputStream), readPos(nullptr) {
  _::WireValue<uint32_t> firstWord[2];

  inputStream.read(firstWord, sizeof(firstWord));

  uint segmentCount = firstWord[0].get() + 1;
  uint segment0Size = segmentCount == 0 ? 0 : firstWord[1].get();

  size_t totalWords = segment0Size;

  // Reject messages with too many segments for security reasons.
  KJ_REQUIRE(segmentCount < 512, "Message has too many segments.") {
    segmentCount = 1;
    segment0Size = 1;
    break;
  }

  // Read sizes for all segments except the first.  Include padding if necessary.
#if _MSC_VER
  size_t nbytes = (segmentCount & ~1) * sizeof(_::WireValue<uint32_t>);
  _::WireValue<uint32_t> *moreSizes = static_cast<_::WireValue<uint32_t> *>(_alloca(nbytes));
#else
  _::WireValue<uint32_t> moreSizes[segmentCount & ~1];
#endif
  if (segmentCount > 1) {
#if _MSC_VER
    inputStream.read(moreSizes, nbytes);
#else
    inputStream.read(moreSizes, sizeof(moreSizes));
#endif
    for (uint i = 0; i < segmentCount - 1; i++) {
      totalWords += moreSizes[i].get();
    }
  }

  // Don't accept a message which the receiver couldn't possibly traverse without hitting the
  // traversal limit.  Without this check, a malicious client could transmit a very large segment
  // size to make the receiver allocate excessive space and possibly crash.
  KJ_REQUIRE(totalWords <= options.traversalLimitInWords,
             "Message is too large.  To increase the limit on the receiving end, see "
             "capnp::ReaderOptions.") {
    segmentCount = 1;
    segment0Size = kj::min(segment0Size, options.traversalLimitInWords);
    totalWords = segment0Size;
    break;
  }

  if (scratchSpace.size() < totalWords) {
    // TODO(perf):  Consider allocating each segment as a separate chunk to reduce memory
    //   fragmentation.
    ownedSpace = kj::heapArray<word>(totalWords);
    scratchSpace = ownedSpace;
  }

  segment0 = scratchSpace.slice(0, segment0Size);

  if (segmentCount > 1) {
    moreSegments = kj::heapArray<kj::ArrayPtr<const word>>(segmentCount - 1);
    size_t offset = segment0Size;

    for (uint i = 0; i < segmentCount - 1; i++) {
      uint segmentSize = moreSizes[i].get();
      moreSegments[i] = scratchSpace.slice(offset, offset + segmentSize);
      offset += segmentSize;
    }
  }

  if (segmentCount == 1) {
    inputStream.read(scratchSpace.begin(), totalWords * sizeof(word));
  } else if (segmentCount > 1) {
    readPos = reinterpret_cast<byte*>(scratchSpace.begin());
    readPos += inputStream.read(readPos, segment0Size * sizeof(word), totalWords * sizeof(word));
  }
}

InputStreamMessageReader::~InputStreamMessageReader() noexcept(false) {
  if (readPos != nullptr) {
    unwindDetector.catchExceptionsIfUnwinding([&]() {
      // Note that lazy reads only happen when we have multiple segments, so moreSegments.back() is
      // valid.
      const byte* allEnd = reinterpret_cast<const byte*>(moreSegments.back().end());
      inputStream.skip(allEnd - readPos);
    });
  }
}

kj::ArrayPtr<const word> InputStreamMessageReader::getSegment(uint id) {
  if (id > moreSegments.size()) {
    return nullptr;
  }

  kj::ArrayPtr<const word> segment = id == 0 ? segment0 : moreSegments[id - 1];

  if (readPos != nullptr) {
    // May need to lazily read more data.
    const byte* segmentEnd = reinterpret_cast<const byte*>(segment.end());
    if (readPos < segmentEnd) {
      // Note that lazy reads only happen when we have multiple segments, so moreSegments.back() is
      // valid.
      const byte* allEnd = reinterpret_cast<const byte*>(moreSegments.back().end());
      readPos += inputStream.read(readPos, segmentEnd - readPos, allEnd - readPos);
    }
  }

  return segment;
}

// -------------------------------------------------------------------

void writeMessage(kj::OutputStream& output, kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  KJ_REQUIRE(segments.size() > 0, "Tried to serialize uninitialized message.");

#if _MSC_VER
  size_t nbytes = ((segments.size() + 2) & ~size_t(1)) * sizeof(_::WireValue<uint32_t>);
  _::WireValue<uint32_t> *table = static_cast<_::WireValue<uint32_t> *>(_alloca(nbytes));
#else
  _::WireValue<uint32_t> table[(segments.size() + 2) & ~size_t(1)];
#endif

  // We write the segment count - 1 because this makes the first word zero for single-segment
  // messages, improving compression.  We don't bother doing this with segment sizes because
  // one-word segments are rare anyway.
  table[0].set(segments.size() - 1);
  for (uint i = 0; i < segments.size(); i++) {
    table[i + 1].set(segments[i].size());
  }
  if (segments.size() % 2 == 0) {
    // Set padding byte.
    table[segments.size() + 1].set(0);
  }

  KJ_STACK_ARRAY(kj::ArrayPtr<const byte>, pieces, segments.size() + 1, 4, 32);
#if _MSC_VER
  pieces[0] = kj::arrayPtr(reinterpret_cast<byte*>(table), nbytes);
#else
  pieces[0] = kj::arrayPtr(reinterpret_cast<byte*>(table), sizeof(table));
#endif

  for (uint i = 0; i < segments.size(); i++) {
    pieces[i + 1] = kj::arrayPtr(reinterpret_cast<const byte*>(segments[i].begin()),
                                 reinterpret_cast<const byte*>(segments[i].end()));
  }

  output.write(pieces);
}

// =======================================================================================
StreamFdMessageReader::~StreamFdMessageReader() noexcept(false) {}

void writeMessageToFd(kj::fdtype fd, kj::ArrayPtr<const kj::ArrayPtr<const word>> segments) {
  kj::FdOutputStream stream(fd);
  writeMessage(stream, segments);
}

}  // namespace capnp

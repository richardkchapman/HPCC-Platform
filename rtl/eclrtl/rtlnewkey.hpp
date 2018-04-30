/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#ifndef RTLNEWKEY_INCL
#define RTLNEWKEY_INCL
#include "eclrtl.hpp"

#include "rtlkey.hpp"
#include "rtlrecord.hpp"

BITMASK_ENUM(TransitionMask);

/*
 * The RowFilter class represents a multiple-field filter of a row.
 */

//MORE: This should probably have an RtlRecord member.
class ECLRTL_API RowFilter
{
public:
    void addFilter(const IFieldFilter & filter);
    void addFilter(const RtlRecord & record, const char * filter);
    bool matches(const RtlRow & row) const;

    void createSegmentMonitors(IIndexReadContext *irc);
    //void extractKeyFilter(const RtlRecord & record, IConstArrayOf<IFieldFilter> & keyFilters) const;
    unsigned numFilterFields() const { return filters.ordinality(); }
    const IFieldFilter & queryFilter(unsigned i) const { return filters.item(i); }
    const IFieldFilter *findFilter(unsigned fieldIdx) const;
    const IFieldFilter *extractFilter(unsigned fieldIdx);
    unsigned getNumFieldsRequired() const { return numFieldsRequired; }
    void remapField(unsigned filterIdx, unsigned newFieldNum);
    void recalcFieldsRequired();
    void remove(unsigned idx);
    void clear();
    void appendFilters(IConstArrayOf<IFieldFilter> &_filters);
protected:
    IConstArrayOf<IFieldFilter> filters;
    unsigned numFieldsRequired = 0;
};

//This class represents the current set of values which have been matched in the filter sets.
//A field can either have a valid current value, or it has an index of the next filter range which must match
//for that field.

class ECLRTL_API FilterState
{
public:
    FilterState(const RtlRecord & record, const RowFilter &_filter) : currentRow(record, nullptr), filter(_filter)
    {
    }

    /*
     * Set filter state to the lowest value of the specified (and subsequent) fields. State for earlier fields left unmodified.
     */
    inline void setLow(unsigned fromField)
    {
        for (unsigned field = fromField; field < matchedRanges.length(); field++)
        {
            matchedRanges.replace(0, field);
        }
    }

    inline void setHigh(unsigned fromField)
    {
        for (unsigned field = fromField; field < matchedRanges.length(); field++)
        {
            const IFieldFilter & filter = queryFilter(field);
            matchedRanges.replace(filter.numRanges(), field);
        }
    }

    /*
     * Set filter state to the next value of the specified field. Subsequent fields set to lowest possible value
     * If there is no next value for this field, increment previous field and set this one low, recursively.
     * Return false if there is no next legal value for the entire filter set.
     */

    inline bool incrementKey(unsigned field)
    {
        setLow(field+1);
        for (;;)
        {
            unsigned matchRange = matchedRanges.item(field);
            const IFieldFilter & filter = queryFilter(field);
            matchRange++;
            if (matchRange < filter.numRanges())
                break;
            matchedRanges.replace(0, field);
            if (field == 0)
            {
                eos = true;
                return false;
            }
            field--;
        }
        return true;;
    }
    /*
     * Set filter state to the last value of the current range of the specified field. Subsequent fields set to highest possible value
     */

    inline void endRange()
    {
        for (unsigned field = numMatched; field < matchedRanges.length(); field++)
        {
            const IFieldFilter & filter = queryFilter(field);
            matchedRanges.replace(filter.numRanges(), field);
        }
        //nextSeekIsLE = true;
    }

    void selectFirst()
    {
        eos = false;
        numMatched = 0;
        nextUnmatchedRange = 0;
        while (matchedRanges.length() < filter.numFilterFields())  // MORE - should ignore trailing wild and wilded OPT fields - if not already removed?
        {
            matchedRanges.append(0);
        }
        setLow(0);
    }

    //Compare the incoming row against the current row
    int compareNext(const RtlRow & candidate) const
    {
        if (nextSeekIsGT())  // Filter state is "immediately after this value" rather than "in this range" - what is "this value" ??
        {
            assertex(nextUnmatchedRange == -1U);
            assertex(numMatched != numFilterFields());
            unsigned i;
            int c = 0;
            //Note field numMatched is not matched, but we are searching for the next highest value => loop until <= numMatched
            for (i = 0; i <= numMatched; i++)
            {
                //Use sequential searches for the values that have previously been matched
                c = queryFilter(i).compareRow(candidate, currentRow);
                if (c != 0)
                    return c;
            }

            //If the next value of the trailing field isn't known then no limit can be placed on the
            //subsequent fields.
            //MORE - If it is possible to increment a key value, then that should be optimized somewhere else
            return -1;
        }
        else
        {
            unsigned i;
            for (i = 0; i < numMatched; i++)
            {
                //Use sequential searches for the values that have previously been matched
                int c = queryFilter(i).compareRow(candidate, currentRow);
                if (c != 0)
                    return c;
            }
            unsigned nextRange = nextUnmatchedRange;
            for (; i < numFilterFields(); i++)
            {
                //Compare the row against each of the potential ranges.
                int c = queryFilter(i).compareLowest(candidate, nextRange);
                if (c != 0)
                    return c;
                nextRange = 0;
            }
            return 0;
        }
    }

    //Compare with the row that is larger than the first numMatched fields.

    bool matches() const
    {
        return filter.matches(currentRow);
    }

    unsigned numFilterFields() const { return filter.numFilterFields(); }
    const RtlRow & queryRow() const { return currentRow; }
    /*
     * I have a row - return true if it matches, otherwise update filter state to indicate next possible value to match
     */
    bool setRowForward(const byte * row);
    bool nextSeekIsGT() const { return (nextUnmatchedRange == -1U); }
    bool noMoreMatches() const { return eos; }

protected:
    unsigned matchPos(unsigned i) const { return matchedRanges.item(i); }
    bool isValid(unsigned i) const { return matchPos(i) < queryFilter(i).numRanges(); }
    const IFieldFilter & queryFilter(unsigned i) const { return filter.queryFilter(i); }
    bool findNextRange(unsigned field);

protected:
    const RowFilter &filter;
    RtlDynRow currentRow;
    unsigned numMatched = 0;
    unsigned nextUnmatchedRange = 0;
    UnsignedArray matchedRanges;  // Contains field numbers - surely could be smaller than 4 bytes. Indexed by filter number. Could probably be a byte.
    bool eos = false;
};

interface ISourceRowCursor
{
public:
    //Find the first row that is forward from the search cursor
    virtual const byte * findNext(const FilterState & current) = 0;
    //Find the last row that is in the current match region
    virtual const byte * findLast(const FilterState & current) = 0;
    //select the next row
    virtual const byte * next() = 0;
    //prepare ready for a new set of seeks
    virtual void reset() = 0;
};

class ECLRTL_API KeySearcher : public CInterface  // MORE - does this support OPT filters? Should they be moved into a separate postfilter phase?
{
public:
    KeySearcher(const RtlRecord & _info, const RowFilter & _filter, ISourceRowCursor * _rows) : cursor(_info, _filter), rows(_rows)
    {
    }

    void reset()
    {
        rows->reset();
        firstPending = true;
    }

    bool next()
    {
        if (firstPending)
        {
            cursor.selectFirst();
            firstPending = false;
        }
        else
        {
            const byte * next = rows->next(); // MORE: Return a RtlRow?
            if (!next)
                return false;
            if (cursor.setRowForward(next))
                return true;
        }
        return findGE();
    }

    /*
    void lastInRange()
    {
        assertex(currrent row matches);
        if (cursor.noMoreMatches())
            return false;

        const byte * match;
        match = rows->findNext(cursor); // more - return the row pointer to avoid recalculation
        if (!match)
            return false;
        printf("Maybe match %.5s\n", match);
        if (cursor.setRowForward(match))
        {
            printf("Did match %.5s\n", match);
            return true;
        }
        findLT();
    }
*/
    bool findGE()
    {
        for (;;)
        {
            if (cursor.noMoreMatches())
                return false;
            const byte * match;
            match = rows->findNext(cursor); // more - return the row pointer to avoid recalculation
            if (!match)
                return false;
            printf("Maybe match %.5s\n", match);
            if (cursor.setRowForward(match))
            {
                printf("Did match %.5s\n", match);
                return true;
            }
        }
    }

    const RtlRow & queryRow() const { return cursor.queryRow(); }

protected:
    ISourceRowCursor * rows = nullptr;
    FilterState cursor;
    bool firstPending = true;
};



#endif

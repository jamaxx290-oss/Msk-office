/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <swmodeltestbase.hxx>

#include <algorithm>
#include <map>
#include <vector>

#include <IDocumentLayoutAccess.hxx>
#include <doc.hxx>
#include <ndtxt.hxx>
#include <pam.hxx>
#include <pagefrm.hxx>
#include <rootfrm.hxx>
#include <section.hxx>

using namespace css;

namespace
{
class Test : public SwModelTestBase
{
public:
    Test()
        : SwModelTestBase(u"/sw/qa/extras/ooxmlexport/data/"_ustr)
    {
    }
};

/// Title and page number of a TOC entry, as read from a TOC entry text node.
struct ToxEntry
{
    OUString maTitle;
    sal_Int32 mnPage = 0;
};

/// Collects the "Title\tPage" text nodes of all entries of all TOC sections.
/// The SwTextNode of each entry is returned as well so the numbers can be
/// overwritten for regression tests.
void lcl_collectToxEntries(SwDoc& rDoc, std::vector<ToxEntry>& rEntries,
                           std::vector<SwTextNode*>& rNodes)
{
    rEntries.clear();
    rNodes.clear();

    SwSectionFormats& rSectionFormats = rDoc.GetSections();
    for (size_t i = 0; i < rSectionFormats.size(); ++i)
    {
        SwSection* pSection = rSectionFormats[i]->GetSection();
        SwSectionNode* pSectionNode = rSectionFormats[i]->GetSectionNode();
        if (!pSection || pSection->GetType() != SectionType::ToxContent || !pSectionNode)
            continue;

        const SwNodeOffset nEnd = pSectionNode->EndOfSectionIndex();
        for (SwNodeOffset j = pSectionNode->GetIndex() + SwNodeOffset(1); j < nEnd; ++j)
        {
            SwTextNode* pTextNode = rDoc.GetNodes()[j]->GetTextNode();
            if (!pTextNode)
                continue;
            const OUString sText = pTextNode->GetText();
            const sal_Int32 nTab = sText.lastIndexOf('\t');
            if (nTab < 0)
                continue;

            ToxEntry aEntry;
            aEntry.maTitle = sText.copy(0, nTab);
            aEntry.mnPage = sText.copy(nTab + 1).toInt32();
            CPPUNIT_ASSERT_MESSAGE((OString("TOC entry without a page number: ")
                                    + sText.toUtf8())
                                       .getStr(),
                                   aEntry.mnPage > 0);

            rEntries.push_back(aEntry);
            rNodes.push_back(pTextNode);
        }
    }
}

/// Maps heading text to the page its text frame really lands on in the layout.
std::map<OUString, sal_Int32> lcl_collectHeadingPages(SwDoc& rDoc)
{
    std::map<OUString, sal_Int32> aResult;
    SwNodes& rNodesArray = rDoc.GetNodes();
    const SwNodeOffset nEndOfContent = rNodesArray.GetEndOfContent().GetIndex();
    for (SwNodeIndex aIdx(rNodesArray.GetEndOfExtras(), SwNodeOffset(1));
         aIdx.GetIndex() < nEndOfContent; ++aIdx)
    {
        SwTextNode* pTextNode = aIdx.GetNode().GetTextNode();
        if (!pTextNode || pTextNode->GetAttrOutlineLevel() <= 0)
            continue;

        SwContentFrame* pFrame = pTextNode->getLayoutFrame(nullptr);
        CPPUNIT_ASSERT_MESSAGE((OString("no layout frame for heading ")
                                + pTextNode->GetText().toUtf8())
                                   .getStr(),
                               pFrame);
        aResult[pTextNode->GetText()] = pFrame->FindPageFrame()->GetVirtPageNum();
    }
    return aResult;
}

/// Overwrites the page number of every TOC entry with rStale, so a fix that
/// fails to refresh the numbers is guaranteed to see the stale value.
void lcl_stompToxPageNumbers(const std::vector<SwTextNode*>& rNodes, const OUString& rStale)
{
    for (SwTextNode* pTextNode : rNodes)
    {
        const OUString sText = pTextNode->GetText();
        const sal_Int32 nTab = sText.lastIndexOf('\t');
        CPPUNIT_ASSERT(nTab >= 0);
        pTextNode->ReplaceText(SwPosition(*pTextNode, nTab + 1), sText.getLength() - (nTab + 1),
                               rStale);
    }
}

/// Asserts that each collected TOC entry carries the real layout page of its
/// heading.
void lcl_assertToxMatchesLayout(SwDoc& rDoc, const char* pMessage)
{
    std::vector<ToxEntry> aEntries;
    std::vector<SwTextNode*> aNodes;
    lcl_collectToxEntries(rDoc, aEntries, aNodes);
    CPPUNIT_ASSERT_MESSAGE((OString(pMessage) + ": no TOC entries collected").getStr(),
                           !aEntries.empty());

    const std::map<OUString, sal_Int32> aActualPages = lcl_collectHeadingPages(rDoc);
    CPPUNIT_ASSERT_MESSAGE((OString(pMessage) + ": no headings found in the layout").getStr(),
                           !aActualPages.empty());
    for (const ToxEntry& rEntry : aEntries)
    {
        CPPUNIT_ASSERT_MESSAGE((OString(pMessage) + ": no heading found for TOC entry "
                                + rEntry.maTitle.toUtf8())
                                   .getStr(),
                               aActualPages.count(rEntry.maTitle));
        CPPUNIT_ASSERT_EQUAL_MESSAGE((OString(pMessage) + ": TOC number is not the real page for "
                                      + rEntry.maTitle.toUtf8())
                                         .getStr(),
                                     aActualPages.at(rEntry.maTitle), rEntry.mnPage);
    }
}

/// Realistic book: TOC field with stale numbers, front matter, chapters with
/// page breaks, second-level sections, an image and a header/footer. Verifies
/// that the TOC page numbers correspond to the real pagination, both after
/// load and after a save/reload cycle.
CPPUNIT_TEST_FIXTURE(Test, testManuskriptaTocPageNumbersRoundtrip)
{
    createSwDoc("manuskripta_book_toc.docx");

    lcl_assertToxMatchesLayout(*getSwDoc(), "right after load");

    saveAndReload(TestFilter::DOCX);

    lcl_assertToxMatchesLayout(*getSwDoc(), "after save and reload");
}

/// Writerfilter schedules a TOC update on import (SwDoc::SetUpdateTOX). A
/// headless consumer never gets a view that would consume the flag, so the
/// export has to refresh the numbers before writing the stored field result.
/// Stomp the numbers, set the flag, export and make sure no stale number
/// survives in the written field.
CPPUNIT_TEST_FIXTURE(Test, testManuskriptaExportResolvesStaleTocNumbers)
{
    createSwDoc("manuskripta_book_toc.docx");
    SwDoc* pDoc = getSwDoc();

    std::vector<SwTextNode*> aNodes;
    std::vector<ToxEntry> aEntries;
    lcl_collectToxEntries(*pDoc, aEntries, aNodes);
    CPPUNIT_ASSERT(!aNodes.empty());

    lcl_stompToxPageNumbers(aNodes, u"77"_ustr);
    const std::map<OUString, sal_Int32> aActualPages = lcl_collectHeadingPages(*pDoc);

    // Simulate what writerfilter does on import when a TOC field is imported.
    pDoc->SetUpdateTOX(true);

    save(TestFilter::DOCX);

    xmlDocUniquePtr pXmlDoc = parseExport(u"word/document.xml"_ustr);
    CPPUNIT_ASSERT(pXmlDoc);

    // The stomped number must not survive anywhere in the exported body: the
    // update has to have replaced all stored field results.
    xmlXPathObjectPtr pXmlObj = getXPathNode(pXmlDoc, "//w:t");
    CPPUNIT_ASSERT(pXmlObj);
    std::vector<OString> aTexts;
    for (sal_Int32 i = 0; i < static_cast<sal_Int32>(xmlXPathNodeSetGetLength(pXmlObj->nodesetval));
         ++i)
    {
        xmlChar* pContent = xmlNodeGetContent(pXmlObj->nodesetval->nodeTab[i]);
        OString sText(reinterpret_cast<const char*>(pContent));
        xmlFree(pContent);
        CPPUNIT_ASSERT_MESSAGE((OString("stale TOC page number was exported: ") + sText).getStr(),
                               sText != "77");
        aTexts.push_back(sText);
    }

    // The entry for the foreword must be followed by its real page number.
    xmlXPathFreeObject(pXmlObj);
    const auto it = std::find(aTexts.begin(), aTexts.end(), OString("Foreword"));
    CPPUNIT_ASSERT_MESSAGE("TOC entry for Foreword missing from the export", it != aTexts.end());
    CPPUNIT_ASSERT_MESSAGE("no page number run after the Foreword entry", it + 1 != aTexts.end());
    const OString sExpected
        = OUStringToOString(OUString::number(aActualPages.at(u"Foreword"_ustr)),
                            RTL_TEXTENCODING_ASCII_US);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("exported TOC number does not match the real page", sExpected,
                                 *(it + 1));
}

/// SwDoc::UpdateAllIndexes() must re-resolve the numbers of all indexes from
/// the current layout, without any shell or view being involved.
CPPUNIT_TEST_FIXTURE(Test, testManuskriptaUpdateAllIndexes)
{
    createSwDoc("manuskripta_book_toc.docx");
    SwDoc* pDoc = getSwDoc();

    std::vector<SwTextNode*> aNodes;
    std::vector<ToxEntry> aEntries;
    lcl_collectToxEntries(*pDoc, aEntries, aNodes);
    CPPUNIT_ASSERT(!aNodes.empty());

    lcl_stompToxPageNumbers(aNodes, u"77"_ustr);

    pDoc->UpdateAllIndexes();

    lcl_assertToxMatchesLayout(*pDoc, "after UpdateAllIndexes");
}

} // end of anonymous namespace

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

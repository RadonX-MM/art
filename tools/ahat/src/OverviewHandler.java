/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.ahat;

import com.android.tools.perflib.heap.Heap;
import java.io.IOException;
import java.io.File;
import java.util.Collections;
import java.util.List;

class OverviewHandler implements AhatHandler {

  private static final String OVERVIEW_ID = "overview";

  private AhatSnapshot mSnapshot;
  private File mHprof;

  public OverviewHandler(AhatSnapshot snapshot, File hprof) {
    mSnapshot = snapshot;
    mHprof = hprof;
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    doc.title("Overview");

    doc.section("General Information");
    doc.descriptions();
    doc.description(
        DocString.text("ahat version"),
        DocString.format("ahat-%s", OverviewHandler.class.getPackage().getImplementationVersion()));
    doc.description(DocString.text("hprof file"), DocString.text(mHprof.toString()));
    doc.end();

    doc.section("Heap Sizes");
    printHeapSizes(doc, query);

    DocString menu = new DocString();
    menu.appendLink(DocString.uri("rooted"), DocString.text("Rooted"));
    menu.append(" - ");
    menu.appendLink(DocString.uri("site"), DocString.text("Allocations"));
    menu.append(" - ");
    menu.appendLink(DocString.uri("help"), DocString.text("Help"));
    doc.big(menu);
  }

  private void printHeapSizes(Doc doc, Query query) {
    List<Object> dummy = Collections.singletonList(null);

    HeapTable.TableConfig<Object> table = new HeapTable.TableConfig<Object>() {
      public String getHeapsDescription() {
        return "Bytes Retained by Heap";
      }

      public long getSize(Object element, Heap heap) {
        return mSnapshot.getHeapSize(heap);
      }

      public List<HeapTable.ValueConfig<Object>> getValueConfigs() {
        return Collections.emptyList();
      }
    };
    HeapTable.render(doc, query, OVERVIEW_ID, table, mSnapshot, dummy);
  }
}


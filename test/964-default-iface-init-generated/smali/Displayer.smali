# /*
#  * Copyright (C) 2015 The Android Open Source Project
#  *
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  *      http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
#  */
#
# // This class is b/c java does not allow static {} blocks in interfaces.
# public class Displayer {
#   public Displayer(String type) {
#       System.out.println("initialization of " + type);
#   }
#   public void touch() {
#       return;
#   }
# }

.class public LDisplayer;
.super Ljava/lang/Object;

.method public constructor <init>(Ljava/lang/String;)V
    .locals 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    const-string v0, "initialization of "
    invoke-virtual {v0, p1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    return-void
.end method

.method public touch()V
    .locals 0
    return-void
.end method


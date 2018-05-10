package com.jbloit.androidlinkaudio

/**
 * Created by bloit on 07/05/2018.
 */


object AudioEngine {
    private var mEngineHandle: Long = 0
    external fun createEngine(): Long
    external fun deleteEngine(engineHandle: Long)
    external fun linkEnable(engineHandle: Long, enableFlag: Boolean)
    external fun createStream(engineHandle: Long)
    external fun playAudio(engineHandle: Long, playFlag: Boolean)

    fun create(): Boolean{
        if (mEngineHandle == 0L) {
            mEngineHandle = createEngine()
        }
        return mEngineHandle != 0L
    }

    fun linkEnable(flag: Boolean){
        if (mEngineHandle != 0L) {
            linkEnable(mEngineHandle, flag)
        }
    }

    fun playAudio(flag: Boolean){
        if (mEngineHandle != 0L) {
            playAudio(mEngineHandle, flag)
        }
    }

    fun createAudioStream(){
        if (mEngineHandle != 0L) {
            createStream(mEngineHandle)
        }
    }

    fun delete() {
        if (mEngineHandle != 0L) {
            deleteEngine(mEngineHandle)
        }
        mEngineHandle = 0
    }
}
package com.fio;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class FioActivity extends Activity {
    static {
        System.loadLibrary("fio");
    }

    private native String runFio();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setText(runFio());
        setContentView(tv);
    }
}
